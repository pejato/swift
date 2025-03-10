//===--- ModuleContentsWriter.cpp - Walk module decls to print ObjC/C++ ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ModuleContentsWriter.h"

#include "ClangSyntaxPrinter.h"
#include "DeclAndTypePrinter.h"
#include "OutputLanguageMode.h"
#include "PrimitiveTypeMapping.h"
#include "PrintClangValueType.h"
#include "PrintSwiftToClangCoreScaffold.h"
#include "SwiftToClangInteropContext.h"

#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Module.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/AST/TypeDeclFinder.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Strings.h"

#include "clang/AST/Decl.h"
#include "clang/Basic/Module.h"

#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::objc_translation;

using DelayedMemberSet = DeclAndTypePrinter::DelayedMemberSet;

/// Returns true if \p decl represents an <os/object.h> type.
static bool isOSObjectType(const clang::Decl *decl) {
  auto *named = dyn_cast_or_null<clang::NamedDecl>(decl);
  if (!named)
    return false;
  return !DeclAndTypePrinter::maybeGetOSObjectBaseName(named).empty();
}

namespace {
class ReferencedTypeFinder : public TypeDeclFinder {
  friend TypeDeclFinder;

  llvm::function_ref<void(ReferencedTypeFinder &, const TypeDecl *)> Callback;
  bool NeedsDefinition = false;

  explicit ReferencedTypeFinder(decltype(Callback) callback)
    : Callback(callback) {}

  Action visitNominalType(NominalType *nominal) override {
    Callback(*this, nominal->getDecl());
    return Action::SkipChildren;
  }

  Action visitTypeAliasType(TypeAliasType *aliasTy) override {
    if (aliasTy->getDecl()->hasClangNode() &&
        !aliasTy->getDecl()->isCompatibilityAlias()) {
      Callback(*this, aliasTy->getDecl());
    } else {
      Type(aliasTy->getSinglyDesugaredType()).walk(*this);
    }
    return Action::SkipChildren;
  }

  /// Returns true if \p paramTy has any constraints other than being
  /// class-bound ("conforms to" AnyObject).
  static bool isConstrained(GenericSignature sig,
                            GenericTypeParamType *paramTy) {
    if (sig->getSuperclassBound(paramTy))
      return true;

    return !sig->getRequiredProtocols(paramTy).empty();
  }

  Action visitBoundGenericType(BoundGenericType *boundGeneric) override {
    auto *decl = boundGeneric->getDecl();

    NeedsDefinition = true;
    Callback(*this, decl);
    NeedsDefinition = false;

    bool isObjCGeneric = decl->hasClangNode();
    auto sig = decl->getGenericSignature();

    for_each(boundGeneric->getGenericArgs(),
             sig.getInnermostGenericParams(),
             [&](Type argTy, GenericTypeParamType *paramTy) {
      // FIXME: I think there's a bug here with recursive generic types.
      if (isObjCGeneric && isConstrained(sig, paramTy))
        NeedsDefinition = true;
      argTy.walk(*this);
      NeedsDefinition = false;
    });
    return Action::SkipChildren;
  }

public:
  bool needsDefinition() const {
    return NeedsDefinition;
  }

  static void walk(Type ty, decltype(Callback) callback) {
    ty.walk(ReferencedTypeFinder(callback));
  }
};

class ModuleWriter {
  enum class EmissionState {
    NotYetDefined = 0,
    DefinitionRequested,
    Defined
  };

  raw_ostream &os;
  SmallPtrSetImpl<ImportModuleTy> &imports;
  ModuleDecl &M;

  llvm::DenseMap<const TypeDecl *, std::pair<EmissionState, bool>> seenTypes;
  llvm::DenseSet<const NominalTypeDecl *> seenClangTypes;
  std::vector<const Decl *> declsToWrite;
  DelayedMemberSet delayedMembers;
  PrimitiveTypeMapping typeMapping;
  std::string outOfLineDefinitions;
  llvm::raw_string_ostream outOfLineDefinitionsOS;
  DeclAndTypePrinter printer;
  OutputLanguageMode outputLangMode;
  bool dependsOnStdlib = false;

public:
  ModuleWriter(raw_ostream &os, raw_ostream &prologueOS,
               llvm::SmallPtrSetImpl<ImportModuleTy> &imports, ModuleDecl &mod,
               SwiftToClangInteropContext &interopContext, AccessLevel access,
               bool requiresExposedAttribute, OutputLanguageMode outputLang)
      : os(os), imports(imports), M(mod),
        outOfLineDefinitionsOS(outOfLineDefinitions),
        printer(M, os, prologueOS, outOfLineDefinitionsOS, delayedMembers,
                typeMapping, interopContext, access, requiresExposedAttribute,
                outputLang),
        outputLangMode(outputLang) {}

  PrimitiveTypeMapping &getTypeMapping() { return typeMapping; }

  /// Returns true if a Stdlib dependency was seen during the emission of this module.
  bool isStdlibRequired() const {
    return dependsOnStdlib;
  }

  /// Returns true if we added the decl's module to the import set, false if
  /// the decl is a local decl.
  ///
  /// The standard library is special-cased: we assume that any types from it
  /// will be handled explicitly rather than needing an explicit @import.
  bool addImport(const Decl *D) {
    ModuleDecl *otherModule = D->getModuleContext();

    if (otherModule == &M)
      return false;
    if (otherModule->isStdlibModule()) {
      dependsOnStdlib = true;
      return true;
    } else if (otherModule->isBuiltinModule())
      return true;
    // Don't need a module for SIMD types in C.
    if (otherModule->getName() == M.getASTContext().Id_simd)
      return true;

    // If there's a Clang node, see if it comes from an explicit submodule.
    // Import that instead, looking through any implicit submodules.
    if (auto clangNode = D->getClangNode()) {
      auto importer =
        static_cast<ClangImporter *>(M.getASTContext().getClangModuleLoader());
      if (const auto *clangModule = importer->getClangOwningModule(clangNode)) {
        while (clangModule && !clangModule->IsExplicit)
          clangModule = clangModule->Parent;
        if (clangModule) {
          imports.insert(clangModule);
          return true;
        }
      }
    }

    if (outputLangMode == OutputLanguageMode::Cxx) {
      // Only add C++ imports in C++ mode for now.
      if (!D->hasClangNode())
        return true;
      if (otherModule->getName().str() == CLANG_HEADER_MODULE_NAME)
        return true;
    }

    imports.insert(otherModule);
    return true;
  }

  bool hasBeenRequested(const TypeDecl *D) const {
    return seenTypes.lookup(D).first >= EmissionState::DefinitionRequested;
  }

  bool tryRequire(const TypeDecl *D) {
    if (addImport(D)) {
      seenTypes[D] = { EmissionState::Defined, true };
      return true;
    }
    auto &state = seenTypes[D];
    return state.first == EmissionState::Defined;
  }

  bool require(const TypeDecl *D) {
    if (addImport(D)) {
      seenTypes[D] = { EmissionState::Defined, true };
      return true;
    }

    auto &state = seenTypes[D];
    switch (state.first) {
    case EmissionState::NotYetDefined:
    case EmissionState::DefinitionRequested:
      state.first = EmissionState::DefinitionRequested;
      declsToWrite.push_back(D);
      return false;
    case EmissionState::Defined:
      return true;
    }

    llvm_unreachable("Unhandled EmissionState in switch.");
  }

  void forwardDeclare(const NominalTypeDecl *NTD,
                      llvm::function_ref<void(void)> Printer) {
    if (NTD->getModuleContext()->isStdlibModule()) {
      if (outputLangMode != OutputLanguageMode::Cxx ||
          !printer.shouldInclude(NTD))
        return;
    }
    auto &state = seenTypes[NTD];
    if (state.second)
      return;
    Printer();
    state.second = true;
  }

  bool forwardDeclare(const ClassDecl *CD) {
    if (!CD->isObjC() ||
        CD->getForeignClassKind() == ClassDecl::ForeignKind::CFType ||
        isOSObjectType(CD->getClangDecl())) {
      return false;
    }
    forwardDeclare(CD, [&]{ os << "@class " << getNameForObjC(CD) << ";\n"; });
    return true;
  }

  void forwardDeclare(const ProtocolDecl *PD) {
    assert(PD->isObjC() ||
           *PD->getKnownProtocolKind() == KnownProtocolKind::Error);
    forwardDeclare(PD, [&]{
      os << "@protocol " << getNameForObjC(PD) << ";\n";
    });
  }

  void forwardDeclare(const EnumDecl *ED) {
    assert(ED->isObjC() || ED->hasClangNode());
    
    forwardDeclare(ED, [&]{
      os << "enum " << getNameForObjC(ED) << " : ";
      printer.print(ED->getRawType());
      os << ";\n";
    });
  }

  void emitReferencedClangTypeMetadata(const NominalTypeDecl *typeDecl) {
    auto it = seenClangTypes.insert(typeDecl);
    if (it.second)
      ClangValueTypePrinter::printClangTypeSwiftGenericTraits(os, typeDecl, &M);
  }

  void forwardDeclareCxxValueTypeIfNeeded(const NominalTypeDecl *NTD) {
    forwardDeclare(NTD,
                   [&]() { ClangValueTypePrinter::forwardDeclType(os, NTD); });
  }

  void forwardDeclareType(const TypeDecl *TD) {
    if (outputLangMode == OutputLanguageMode::Cxx) {
      if (isa<StructDecl>(TD) || isa<EnumDecl>(TD)) {
        auto *NTD = cast<NominalTypeDecl>(TD);
        if (!addImport(NTD))
          forwardDeclareCxxValueTypeIfNeeded(NTD);
        else if (isa<StructDecl>(TD) && NTD->hasClangNode())
          emitReferencedClangTypeMetadata(NTD);
      }
      return;
    }
    if (auto CD = dyn_cast<ClassDecl>(TD)) {
      if (!forwardDeclare(CD)) {
        (void)addImport(CD);
      }
    } else if (auto PD = dyn_cast<ProtocolDecl>(TD)) {
      forwardDeclare(PD);
    } else if (auto TAD = dyn_cast<TypeAliasDecl>(TD)) {
      bool imported = false;
      if (TAD->hasClangNode())
        imported = addImport(TD);
      assert((imported || !TAD->isGeneric()) &&
             "referencing non-imported generic typealias?");
    } else if (addImport(TD)) {
      return;
    } else if (auto ED = dyn_cast<EnumDecl>(TD)) {
      forwardDeclare(ED);
    } else if (isa<GenericTypeParamDecl>(TD)) {
      llvm_unreachable("should not see generic parameters here");
    } else if (isa<AssociatedTypeDecl>(TD)) {
      llvm_unreachable("should not see associated types here");
    } else if (isa<StructDecl>(TD) &&
               TD->getModuleContext()->isStdlibModule()) {
      // stdlib has some @_cdecl functions with structs.
      return;
    } else {
      assert(false && "unknown local type decl");
    }
  }

  bool forwardDeclareMemberTypes(DeclRange members, const Decl *container) {
    PrettyStackTraceDecl
        entry("printing forward declarations needed by members of", container);
    switch (container->getKind()) {
    case DeclKind::Class:
    case DeclKind::Protocol:
    case DeclKind::Extension:
      break;
    case DeclKind::Struct:
    case DeclKind::Enum:
      if (outputLangMode == OutputLanguageMode::Cxx)
        break;
      LLVM_FALLTHROUGH;
    default:
      llvm_unreachable("unexpected container kind");
    }

    bool hadAnyDelayedMembers = false;
    SmallVector<ValueDecl *, 4> nestedTypes;
    for (auto member : members) {
      PrettyStackTraceDecl loopEntry("printing for member", member);
      auto VD = dyn_cast<ValueDecl>(member);
      if (!VD || !printer.shouldInclude(VD))
        continue;

      // Catch nested types and emit their definitions /after/ this class.
      if (isa<TypeDecl>(VD)) {
        // Don't emit nested types that are just implicitly @objc.
        // You should have to opt into this, since they are even less
        // namespaced than usual.
        if (std::any_of(VD->getAttrs().begin(), VD->getAttrs().end(),
                        [](const DeclAttribute *attr) {
                          return isa<ObjCAttr>(attr) && !attr->isImplicit();
                        })) {
          nestedTypes.push_back(VD);
        }
        continue;
      }

      bool needsToBeIndividuallyDelayed = false;
      ReferencedTypeFinder::walk(VD->getInterfaceType(),
                                 [&](ReferencedTypeFinder &finder,
                                     const TypeDecl *TD) {
        PrettyStackTraceDecl
            entry("walking its interface type, currently at", TD);
        if (TD == container)
          return;

        // Bridge, if necessary.
        if (outputLangMode != OutputLanguageMode::Cxx)
          TD = printer.getObjCTypeDecl(TD);

        if (finder.needsDefinition() && isa<NominalTypeDecl>(TD)) {
          // We can delay individual members of classes; do so if necessary.
          if (isa<ClassDecl>(container)) {
            if (!tryRequire(TD)) {
              needsToBeIndividuallyDelayed = true;
              hadAnyDelayedMembers = true;
            }
            return;
          }

          // Extensions can always be delayed wholesale.
          if (isa<ExtensionDecl>(container)) {
            if (!require(TD))
              hadAnyDelayedMembers = true;
            return;
          }

          // Protocols should be delayed wholesale unless we might have a cycle.
          if (auto *proto = dyn_cast<ProtocolDecl>(container)) {
            if (!hasBeenRequested(proto) || !hasBeenRequested(TD)) {
              if (!require(TD))
                hadAnyDelayedMembers = true;
              return;
            }
          }

          // Otherwise, we have a cyclic dependency. Give up and continue with
          // regular forward-declarations even though this will lead to an
          // error; there's nothing we can do here.
          // FIXME: It would be nice to diagnose this.
        }

        forwardDeclareType(TD);
      });

      if (needsToBeIndividuallyDelayed) {
        assert(isa<ClassDecl>(container));
        delayedMembers.insert(VD);
      }
    }

    declsToWrite.insert(declsToWrite.end()-1, nestedTypes.rbegin(),
                        nestedTypes.rend());

    // Separate forward declarations from the class itself.
    return !hadAnyDelayedMembers;
  }

  bool writeClass(const ClassDecl *CD) {
    if (addImport(CD))
      return true;

    if (seenTypes[CD].first == EmissionState::Defined)
      return true;

    bool allRequirementsSatisfied = true;

    const ClassDecl *superclass = nullptr;
    if ((superclass = CD->getSuperclassDecl())) {
      allRequirementsSatisfied &= require(superclass);
    }
    if (outputLangMode != OutputLanguageMode::Cxx) {
      for (auto proto :
           CD->getLocalProtocols(ConformanceLookupKind::OnlyExplicit))
        if (printer.shouldInclude(proto))
          allRequirementsSatisfied &= require(proto);
    }
    if (!allRequirementsSatisfied)
      return false;

    (void)forwardDeclareMemberTypes(CD->getMembers(), CD);
    seenTypes[CD] = { EmissionState::Defined, true };
    os << '\n';
    printer.print(CD);
    return true;
  }

  bool writeFunc(const FuncDecl *FD) {
    if (addImport(FD))
      return true;

    PrettyStackTraceDecl entry(
        "printing forward declarations needed by function", FD);
    ReferencedTypeFinder::walk(
        FD->getInterfaceType(),
        [&](ReferencedTypeFinder &finder, const TypeDecl *TD) {
          PrettyStackTraceDecl entry("walking its interface type, currently at",
                                     TD);
          forwardDeclareType(TD);
        });

    os << '\n';
    printer.print(FD);
    return true;
  }

  bool writeStruct(const StructDecl *SD) {
    if (addImport(SD))
      return true;
    if (outputLangMode == OutputLanguageMode::Cxx) {
      (void)forwardDeclareMemberTypes(SD->getMembers(), SD);
      for (const auto *ed :
           printer.getInteropContext().getExtensionsForNominalType(SD)) {
        (void)forwardDeclareMemberTypes(ed->getMembers(), SD);
      }
      forwardDeclareCxxValueTypeIfNeeded(SD);
    }
    printer.print(SD);
    return true;
  }

  bool writeProtocol(const ProtocolDecl *PD) {
    if (addImport(PD))
      return true;

    if (seenTypes[PD].first == EmissionState::Defined)
      return true;

    bool allRequirementsSatisfied = true;

    for (auto proto : PD->getInheritedProtocols()) {
      if (printer.shouldInclude(proto)) {
        assert(proto->isObjC());
        allRequirementsSatisfied &= require(proto);
      }
    }

    if (!allRequirementsSatisfied)
      return false;

    if (!forwardDeclareMemberTypes(PD->getMembers(), PD))
      return false;

    seenTypes[PD] = { EmissionState::Defined, true };
    os << '\n';
    printer.print(PD);
    return true;
  }

  bool writeExtension(const ExtensionDecl *ED) {
    if (printer.isEmptyExtensionDecl(ED))
      return true;

    bool allRequirementsSatisfied = true;

    const ClassDecl *CD = ED->getSelfClassDecl();
    allRequirementsSatisfied &= require(CD);
    for (auto proto : ED->getLocalProtocols())
      if (printer.shouldInclude(proto))
        allRequirementsSatisfied &= require(proto);

    if (!allRequirementsSatisfied)
      return false;

    // This isn't rolled up into the previous set of requirements because
    // it /also/ prints forward declarations, and the header is a little
    // prettier if those are as close as possible to the necessary extension.
    if (!forwardDeclareMemberTypes(ED->getMembers(), ED))
      return false;

    os << '\n';
    printer.print(ED);
    return true;
  }
  
  bool writeEnum(const EnumDecl *ED) {
    if (addImport(ED))
      return true;

    if (outputLangMode == OutputLanguageMode::Cxx) {
      forwardDeclareMemberTypes(ED->getMembers(), ED);
      forwardDeclareCxxValueTypeIfNeeded(ED);
    }

    if (seenTypes[ED].first == EmissionState::Defined)
      return true;
    
    seenTypes[ED] = {EmissionState::Defined, true};
    printer.print(ED);

    ASTContext &ctx = M.getASTContext();

    SmallVector<ProtocolConformance *, 1> conformances;
    auto errorTypeProto = ctx.getProtocol(KnownProtocolKind::Error);
    if (outputLangMode != OutputLanguageMode::Cxx
        && ED->lookupConformance(errorTypeProto, conformances)) {
      bool hasDomainCase = std::any_of(ED->getAllElements().begin(),
                                       ED->getAllElements().end(),
                                       [](const EnumElementDecl *elem) {
        return elem->getBaseIdentifier().str() == "Domain";
      });
      if (!hasDomainCase) {
        os << "static NSString * _Nonnull const " << getNameForObjC(ED)
           << "Domain = @\"" << getErrorDomainStringForObjC(ED) << "\";\n";
      }
    }

    return true;
  }

  void write() {
    SmallVector<Decl *, 64> decls;
    M.getTopLevelDecls(decls);

    auto newEnd = std::remove_if(decls.begin(), decls.end(),
                                 [this](const Decl *D) -> bool {
      if (auto VD = dyn_cast<ValueDecl>(D))
        return !printer.shouldInclude(VD);

      if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        if (outputLangMode == OutputLanguageMode::Cxx)
          return false;
        auto baseClass = ED->getSelfClassDecl();
        return !baseClass || !printer.shouldInclude(baseClass) ||
               baseClass->isForeign();
      }
      return true;
    });
    decls.erase(newEnd, decls.end());

    // REVERSE sort the decls, since we are going to copy them onto a stack.
    llvm::array_pod_sort(decls.begin(), decls.end(),
                         [](Decl * const *lhs, Decl * const *rhs) -> int {
      enum : int {
        Ascending = -1,
        Equivalent = 0,
        Descending = 1,
      };

      assert(*lhs != *rhs && "duplicate top-level decl");

      auto getSortName = [](const Decl *D) -> StringRef {
        if (auto VD = dyn_cast<ValueDecl>(D))
          return VD->getBaseName().userFacingName();

        if (auto ED = dyn_cast<ExtensionDecl>(D)) {
          auto baseClass = ED->getSelfClassDecl();
          if (!baseClass)
              return ED->getExtendedNominal()->getName().str();
          return baseClass->getName().str();
        }
        llvm_unreachable("unknown top-level ObjC decl");
      };

      // Sort by names.
      int result = getSortName(*rhs).compare(getSortName(*lhs));
      if (result != 0)
        return result;

      // Prefer value decls to extensions.
      assert(!(isa<ValueDecl>(*lhs) && isa<ValueDecl>(*rhs)));
      if (isa<ValueDecl>(*lhs) && !isa<ValueDecl>(*rhs))
        return Descending;
      if (!isa<ValueDecl>(*lhs) && isa<ValueDecl>(*rhs))
        return Ascending;

      // Break ties in extensions by putting smaller extensions last (in reverse
      // order).
      // FIXME: This will end up taking linear time.
      auto lhsMembers = cast<ExtensionDecl>(*lhs)->getMembers();
      auto rhsMembers = cast<ExtensionDecl>(*rhs)->getMembers();
      unsigned numLHSMembers = std::distance(lhsMembers.begin(),
                                             lhsMembers.end());
      unsigned numRHSMembers = std::distance(rhsMembers.begin(),
                                             rhsMembers.end());
      if (numLHSMembers != numRHSMembers)
        return numLHSMembers < numRHSMembers ? Descending : Ascending;

      // Or the extension with fewer protocols.
      auto lhsProtos = cast<ExtensionDecl>(*lhs)->getLocalProtocols();
      auto rhsProtos = cast<ExtensionDecl>(*rhs)->getLocalProtocols();
      if (lhsProtos.size() != rhsProtos.size())
        return lhsProtos.size() < rhsProtos.size() ? Descending : Ascending;

      // If that fails, arbitrarily pick the extension whose protocols are
      // alphabetically first.
      auto mismatch =
        std::mismatch(lhsProtos.begin(), lhsProtos.end(), rhsProtos.begin(),
                      [] (const ProtocolDecl *nextLHSProto,
                          const ProtocolDecl *nextRHSProto) {
        return nextLHSProto->getName() != nextRHSProto->getName();
      });
      if (mismatch.first == lhsProtos.end())
        return Equivalent;
      StringRef lhsProtoName = (*mismatch.first)->getName().str();
      return lhsProtoName.compare((*mismatch.second)->getName().str());
    });

    assert(declsToWrite.empty());
    declsToWrite.assign(decls.begin(), decls.end());

    if (outputLangMode == OutputLanguageMode::Cxx) {
      for (const Decl *D : declsToWrite) {
        if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
          const auto *type = ED->getExtendedNominal();
          if (isa<StructDecl>(type))
            printer.getInteropContext().recordExtensions(type, ED);
        }
      }
    }

    while (!declsToWrite.empty()) {
      const Decl *D = declsToWrite.back();
      bool success = true;

      if (auto ED = dyn_cast<EnumDecl>(D)) {
        success = writeEnum(ED);
      } else if (auto CD = dyn_cast<ClassDecl>(D)) {
        success = writeClass(CD);
      } else if (outputLangMode == OutputLanguageMode::Cxx) {
        if (auto FD = dyn_cast<FuncDecl>(D))
          success = writeFunc(FD);
        if (auto SD = dyn_cast<StructDecl>(D))
          success = writeStruct(SD);
        // FIXME: Warn on unsupported exported decl.
      } else if (isa<ValueDecl>(D)) {
        if (auto PD = dyn_cast<ProtocolDecl>(D))
          success = writeProtocol(PD);
        else if (auto ED = dyn_cast<FuncDecl>(D))
          success = writeFunc(ED);
        else
          llvm_unreachable("unknown top-level ObjC value decl");

      } else if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        success = writeExtension(ED);

      } else {
        llvm_unreachable("unknown top-level ObjC decl");
      }

      if (success) {
        assert(declsToWrite.back() == D);
        os << "\n";
        declsToWrite.pop_back();
      }
    }

    if (!delayedMembers.empty()) {
      auto groupBegin = delayedMembers.begin();
      for (auto i = groupBegin, e = delayedMembers.end(); i != e; ++i) {
        if ((*i)->getDeclContext() != (*groupBegin)->getDeclContext()) {
          printer.printAdHocCategory(make_range(groupBegin, i));
          groupBegin = i;
        }
      }
      printer.printAdHocCategory(make_range(groupBegin, delayedMembers.end()));
    }

    // Print any out of line definitions.
    os << outOfLineDefinitionsOS.str();
  }
};
} // end anonymous namespace

static AccessLevel getRequiredAccess(const ModuleDecl &M) {
  return M.isExternallyConsumed() ? AccessLevel::Public : AccessLevel::Internal;
}

void swift::printModuleContentsAsObjC(
    raw_ostream &os, llvm::SmallPtrSetImpl<ImportModuleTy> &imports,
    ModuleDecl &M, SwiftToClangInteropContext &interopContext) {
  llvm::raw_null_ostream prologueOS;
  ModuleWriter(os, prologueOS, imports, M, interopContext, getRequiredAccess(M),
               /*requiresExposedAttribute=*/false, OutputLanguageMode::ObjC)
      .write();
}

EmittedClangHeaderDependencyInfo swift::printModuleContentsAsCxx(
    raw_ostream &os,
    ModuleDecl &M, SwiftToClangInteropContext &interopContext,
    bool requiresExposedAttribute) {
  std::string moduleContentsBuf;
  llvm::raw_string_ostream moduleOS{moduleContentsBuf};
  std::string modulePrologueBuf;
  llvm::raw_string_ostream prologueOS{modulePrologueBuf};
  EmittedClangHeaderDependencyInfo info;

  // FIXME: Use getRequiredAccess once @expose is supported.
  ModuleWriter writer(moduleOS, prologueOS, info.imports, M, interopContext,
                      AccessLevel::Public, requiresExposedAttribute,
                      OutputLanguageMode::Cxx);
  writer.write();
  info.dependsOnStandardLibrary = writer.isStdlibRequired();
  if (M.isStdlibModule()) {
    // Embed an overlay for the standard library.
    ClangSyntaxPrinter(moduleOS).printIncludeForShimHeader(
        "_SwiftStdlibCxxOverlay.h");
  }

  os << "#ifndef SWIFT_PRINTED_CORE\n";
  os << "#define SWIFT_PRINTED_CORE\n";
  printSwiftToClangCoreScaffold(interopContext, M.getASTContext(),
                                writer.getTypeMapping(), os);
  os << "#endif\n";

  // FIXME: refactor.
  if (!prologueOS.str().empty()) {
    // FIXME: This is a workaround for prologue being emitted outside of
    // __cplusplus.
    if (!M.isStdlibModule())
      os << "#endif\n";
    os << "#ifdef __cplusplus\n";
    os << "namespace ";
    M.ValueDecl::getName().print(os);
    os << " __attribute__((swift_private))";
    os << " {\n";
    os << "namespace " << cxx_synthesis::getCxxImplNamespaceName() << " {\n";
    os << "extern \"C\" {\n";
    os << "#endif\n\n";

    os << prologueOS.str();

    if (!M.isStdlibModule())
      os << "\n#ifdef __cplusplus\n";
    os << "}\n";
    os << "}\n";
    os << "}\n";
  }

  // Construct a C++ namespace for the module.
  ClangSyntaxPrinter(os).printNamespace(
      [&](raw_ostream &os) { M.ValueDecl::getName().print(os); },
      [&](raw_ostream &os) { os << moduleOS.str(); },
      ClangSyntaxPrinter::NamespaceTrivia::AttributeSwiftPrivate);
  return info;
}
