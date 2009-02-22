//===--- SemaExprObjC.cpp - Semantic Analysis for ObjC Expressions --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for Objective-C expressions.
//
//===----------------------------------------------------------------------===//

#include "Sema.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ExprObjC.h"
#include "llvm/ADT/SmallString.h"
using namespace clang;

Sema::ExprResult Sema::ParseObjCStringLiteral(SourceLocation *AtLocs, 
                                              ExprTy **strings,
                                              unsigned NumStrings) {
  StringLiteral **Strings = reinterpret_cast<StringLiteral**>(strings);

  // Most ObjC strings are formed out of a single piece.  However, we *can*
  // have strings formed out of multiple @ strings with multiple pptokens in
  // each one, e.g. @"foo" "bar" @"baz" "qux"   which need to be turned into one
  // StringLiteral for ObjCStringLiteral to hold onto.
  StringLiteral *S = Strings[0];
  
  // If we have a multi-part string, merge it all together.
  if (NumStrings != 1) {
    // Concatenate objc strings.
    llvm::SmallString<128> StrBuf;
    llvm::SmallVector<SourceLocation, 8> StrLocs;
    
    for (unsigned i = 0; i != NumStrings; ++i) {
      S = Strings[i];
      
      // ObjC strings can't be wide.
      if (S->isWide()) {
        Diag(S->getLocStart(), diag::err_cfstring_literal_not_string_constant)
          << S->getSourceRange();
        return true;
      }
      
      // Get the string data.
      StrBuf.append(S->getStrData(), S->getStrData()+S->getByteLength());
      
      // Get the locations of the string tokens.
      StrLocs.append(S->tokloc_begin(), S->tokloc_end());
      
      // Free the temporary string.
      S->Destroy(Context);
    }
    
    // Create the aggregate string with the appropriate content and location
    // information.
    S = StringLiteral::Create(Context, &StrBuf[0], StrBuf.size(), false,
                              Context.getPointerType(Context.CharTy),
                              &StrLocs[0], StrLocs.size());
  }
  
  // Verify that this composite string is acceptable for ObjC strings.
  if (CheckObjCString(S))
    return true;

  // Initialize the constant string interface lazily. This assumes
  // the NSConstantString interface is seen in this translation unit.
  QualType Ty = Context.getObjCConstantStringInterface();
  if (!Ty.isNull()) {
    Ty = Context.getPointerType(Ty);
  } else {
    IdentifierInfo *NSIdent = &Context.Idents.get("NSConstantString");
    NamedDecl *IF = LookupName(TUScope, NSIdent, LookupOrdinaryName);
    if (ObjCInterfaceDecl *StrIF = dyn_cast_or_null<ObjCInterfaceDecl>(IF)) {
      Context.setObjCConstantStringInterface(StrIF);
      Ty = Context.getObjCConstantStringInterface();
      Ty = Context.getPointerType(Ty);
    } else {
      // If there is no NSConstantString interface defined then treat constant
      // strings as untyped objects and let the runtime figure it out later.
      Ty = Context.getObjCIdType();
    }
  }
  
  return new (Context) ObjCStringLiteral(S, Ty, AtLocs[0]);
}

Sema::ExprResult Sema::ParseObjCEncodeExpression(SourceLocation AtLoc,
                                                 SourceLocation EncodeLoc,
                                                 SourceLocation LParenLoc,
                                                 TypeTy *ty,
                                                 SourceLocation RParenLoc) {
  QualType EncodedType = QualType::getFromOpaquePtr(ty);

  QualType Ty = Context.getPointerType(Context.CharTy);
  return new (Context) ObjCEncodeExpr(Ty, EncodedType, AtLoc, RParenLoc);
}

Sema::ExprResult Sema::ParseObjCSelectorExpression(Selector Sel,
                                                   SourceLocation AtLoc,
                                                   SourceLocation SelLoc,
                                                   SourceLocation LParenLoc,
                                                   SourceLocation RParenLoc) {
  QualType Ty = Context.getObjCSelType();
  return new (Context) ObjCSelectorExpr(Ty, Sel, AtLoc, RParenLoc);
}

Sema::ExprResult Sema::ParseObjCProtocolExpression(IdentifierInfo *ProtocolId,
                                                   SourceLocation AtLoc,
                                                   SourceLocation ProtoLoc,
                                                   SourceLocation LParenLoc,
                                                   SourceLocation RParenLoc) {
  ObjCProtocolDecl* PDecl = ObjCProtocols[ProtocolId];
  if (!PDecl) {
    Diag(ProtoLoc, diag::err_undeclared_protocol) << ProtocolId;
    return true;
  }
  
  QualType Ty = Context.getObjCProtoType();
  if (Ty.isNull())
    return true;
  Ty = Context.getPointerType(Ty);
  return new (Context) ObjCProtocolExpr(Ty, PDecl, AtLoc, RParenLoc);
}

bool Sema::CheckMessageArgumentTypes(Expr **Args, unsigned NumArgs, 
                                     Selector Sel, ObjCMethodDecl *Method, 
                                     bool isClassMessage,
                                     SourceLocation lbrac, SourceLocation rbrac,
                                     QualType &ReturnType) {  
  if (!Method) {
    // Apply default argument promotion as for (C99 6.5.2.2p6).
    for (unsigned i = 0; i != NumArgs; i++)
      DefaultArgumentPromotion(Args[i]);

    unsigned DiagID = isClassMessage ? diag::warn_class_method_not_found :
                                       diag::warn_inst_method_not_found;
    Diag(lbrac, DiagID)
      << Sel << isClassMessage << SourceRange(lbrac, rbrac);
    ReturnType = Context.getObjCIdType();
    return false;
  }
  
  ReturnType = Method->getResultType();
   
  unsigned NumNamedArgs = Sel.getNumArgs();
  assert(NumArgs >= NumNamedArgs && "Too few arguments for selector!");

  bool anyIncompatibleArgs = false;
  for (unsigned i = 0; i < NumNamedArgs; i++) {
    Expr *argExpr = Args[i];
    assert(argExpr && "CheckMessageArgumentTypes(): missing expression");
    
    QualType lhsType = Method->param_begin()[i]->getType();
    QualType rhsType = argExpr->getType();

    // If necessary, apply function/array conversion. C99 6.7.5.3p[7,8]. 
    if (lhsType->isArrayType())
      lhsType = Context.getArrayDecayedType(lhsType);
    else if (lhsType->isFunctionType())
      lhsType = Context.getPointerType(lhsType);

    AssignConvertType Result = 
      CheckSingleAssignmentConstraints(lhsType, argExpr);
    if (Args[i] != argExpr) // The expression was converted.
      Args[i] = argExpr; // Make sure we store the converted expression.
    
    anyIncompatibleArgs |= 
      DiagnoseAssignmentResult(Result, argExpr->getLocStart(), lhsType, rhsType,
                               argExpr, "sending");
  }

  // Promote additional arguments to variadic methods.
  if (Method->isVariadic()) {
    for (unsigned i = NumNamedArgs; i < NumArgs; ++i)
      DefaultVariadicArgumentPromotion(Args[i], VariadicMethod);
  } else {
    // Check for extra arguments to non-variadic methods.
    if (NumArgs != NumNamedArgs) {
      Diag(Args[NumNamedArgs]->getLocStart(), 
           diag::err_typecheck_call_too_many_args)
        << 2 /*method*/ << Method->getSourceRange()
        << SourceRange(Args[NumNamedArgs]->getLocStart(),
                       Args[NumArgs-1]->getLocEnd());
    }
  }

  return anyIncompatibleArgs;
}

// ActOnClassMessage - used for both unary and keyword messages.
// ArgExprs is optional - if it is present, the number of expressions
// is obtained from Sel.getNumArgs().
Sema::ExprResult Sema::ActOnClassMessage(
  Scope *S,
  IdentifierInfo *receiverName, Selector Sel,
  SourceLocation lbrac, SourceLocation receiverLoc,
  SourceLocation selectorLoc, SourceLocation rbrac, 
  ExprTy **Args, unsigned NumArgs)
{
  assert(receiverName && "missing receiver class name");

  Expr **ArgExprs = reinterpret_cast<Expr **>(Args);
  ObjCInterfaceDecl* ClassDecl = 0;
  bool isSuper = false;
  
  if (receiverName->isStr("super")) {
    if (getCurMethodDecl()) {
      isSuper = true;
      ObjCInterfaceDecl *OID = getCurMethodDecl()->getClassInterface();
      if (!OID)
        return Diag(lbrac, diag::error_no_super_class_message) 
                      << getCurMethodDecl()->getDeclName();
      ClassDecl = OID->getSuperClass();
      if (!ClassDecl)
        return Diag(lbrac, diag::error_no_super_class) << OID->getDeclName();
      if (getCurMethodDecl()->isInstanceMethod()) {
        QualType superTy = Context.getObjCInterfaceType(ClassDecl);
        superTy = Context.getPointerType(superTy);
        ExprResult ReceiverExpr = new (Context) ObjCSuperExpr(SourceLocation(),
                                                              superTy);
        // We are really in an instance method, redirect.
        return ActOnInstanceMessage(ReceiverExpr.get(), Sel, lbrac, 
                                    selectorLoc, rbrac, Args, NumArgs);
      }
      // We are sending a message to 'super' within a class method. Do nothing,
      // the receiver will pass through as 'super' (how convenient:-).
    } else {
      // 'super' has been used outside a method context. If a variable named
      // 'super' has been declared, redirect. If not, produce a diagnostic.
      NamedDecl *SuperDecl = LookupName(S, receiverName, LookupOrdinaryName);
      ValueDecl *VD = dyn_cast_or_null<ValueDecl>(SuperDecl);
      if (VD) {
        ExprResult ReceiverExpr = new (Context) DeclRefExpr(VD, VD->getType(), 
                                                            receiverLoc);
        // We are really in an instance method, redirect.
        return ActOnInstanceMessage(ReceiverExpr.get(), Sel, lbrac, 
                                    selectorLoc, rbrac, Args, NumArgs);
      }
      return Diag(receiverLoc, diag::err_undeclared_var_use) << receiverName;
    }      
  } else
    ClassDecl = getObjCInterfaceDecl(receiverName);
  
  // The following code allows for the following GCC-ism:
  //
  //  typedef XCElementDisplayRect XCElementGraphicsRect;
  //
  //  @implementation XCRASlice
  //  - whatever { // Note that XCElementGraphicsRect is a typedef name.
  //    _sGraphicsDelegate =[[XCElementGraphicsRect alloc] init];
  //  }
  //
  // If necessary, the following lookup could move to getObjCInterfaceDecl().
  if (!ClassDecl) {
    NamedDecl *IDecl = LookupName(TUScope, receiverName, LookupOrdinaryName);
    if (TypedefDecl *OCTD = dyn_cast_or_null<TypedefDecl>(IDecl)) {
      const ObjCInterfaceType *OCIT;
      OCIT = OCTD->getUnderlyingType()->getAsObjCInterfaceType();
      if (!OCIT)
        return Diag(receiverLoc, diag::err_invalid_receiver_to_message);
      ClassDecl = OCIT->getDecl();
    }
  }
  assert(ClassDecl && "missing interface declaration");
  ObjCMethodDecl *Method = 0;
  QualType returnType;
  Method = ClassDecl->lookupClassMethod(Sel);
  
  // If we have an implementation in scope, check "private" methods.
  if (!Method) {
    if (ObjCImplementationDecl *ImpDecl = 
        ObjCImplementations[ClassDecl->getIdentifier()])
      Method = ImpDecl->getClassMethod(Sel);
      
    // Look through local category implementations associated with the class.
    if (!Method) {
      for (unsigned i = 0; i < ObjCCategoryImpls.size() && !Method; i++) {
        if (ObjCCategoryImpls[i]->getClassInterface() == ClassDecl)
          Method = ObjCCategoryImpls[i]->getClassMethod(Sel);
      }
    }
  }
  // Before we give up, check if the selector is an instance method.
  if (!Method)
    Method = ClassDecl->lookupInstanceMethod(Sel);

  if (Method && DiagnoseUseOfDecl(Method, receiverLoc))
    return true;
  
  if (CheckMessageArgumentTypes(ArgExprs, NumArgs, Sel, Method, true, 
                                lbrac, rbrac, returnType))
    return true;

  // If we have the ObjCInterfaceDecl* for the class that is receiving
  // the message, use that to construct the ObjCMessageExpr.  Otherwise
  // pass on the IdentifierInfo* for the class.
  // FIXME: need to do a better job handling 'super' usage within a class 
  // For now, we simply pass the "super" identifier through (which isn't
  // consistent with instance methods.
  if (isSuper)
    return new (Context) ObjCMessageExpr(receiverName, Sel, returnType, Method,
                                         lbrac, rbrac, ArgExprs, NumArgs);
  else
    return new (Context) ObjCMessageExpr(ClassDecl, Sel, returnType, Method,
                                         lbrac, rbrac, ArgExprs, NumArgs);
}

// ActOnInstanceMessage - used for both unary and keyword messages.
// ArgExprs is optional - if it is present, the number of expressions
// is obtained from Sel.getNumArgs().
Sema::ExprResult Sema::ActOnInstanceMessage(ExprTy *receiver, Selector Sel,
                                            SourceLocation lbrac, 
                                            SourceLocation receiverLoc,
                                            SourceLocation rbrac,
                                            ExprTy **Args, unsigned NumArgs) {
  assert(receiver && "missing receiver expression");
  
  Expr **ArgExprs = reinterpret_cast<Expr **>(Args);
  Expr *RExpr = static_cast<Expr *>(receiver);
  QualType returnType;

  QualType ReceiverCType =
    Context.getCanonicalType(RExpr->getType()).getUnqualifiedType();

  // Handle messages to 'super'.
  if (isa<ObjCSuperExpr>(RExpr)) {
    ObjCMethodDecl *Method = 0;
    if (ObjCMethodDecl *CurMeth = getCurMethodDecl()) {
      // If we have an interface in scope, check 'super' methods.
      if (ObjCInterfaceDecl *ClassDecl = CurMeth->getClassInterface())
        if (ObjCInterfaceDecl *SuperDecl = ClassDecl->getSuperClass())
          Method = SuperDecl->lookupInstanceMethod(Sel);
    }

    if (Method && DiagnoseUseOfDecl(Method, receiverLoc))
      return true;

    if (CheckMessageArgumentTypes(ArgExprs, NumArgs, Sel, Method, false,
                                  lbrac, rbrac, returnType))
      return true;
    return new (Context) ObjCMessageExpr(RExpr, Sel, returnType, Method, lbrac,
                                         rbrac, ArgExprs, NumArgs);
  }

  // Handle messages to id.
  if (ReceiverCType == Context.getCanonicalType(Context.getObjCIdType()) ||
      ReceiverCType->getAsBlockPointerType()) {
    ObjCMethodDecl *Method = LookupInstanceMethodInGlobalPool(
                               Sel, SourceRange(lbrac,rbrac));
    if (!Method)
      Method = FactoryMethodPool[Sel].Method;
    if (CheckMessageArgumentTypes(ArgExprs, NumArgs, Sel, Method, false, 
                                  lbrac, rbrac, returnType))
      return true;
    return new (Context) ObjCMessageExpr(RExpr, Sel, returnType, Method, lbrac,
                                         rbrac, ArgExprs, NumArgs);
  }
  
  // Handle messages to Class.
  if (ReceiverCType == Context.getCanonicalType(Context.getObjCClassType())) {
    ObjCMethodDecl *Method = 0;
    if (ObjCMethodDecl *CurMeth = getCurMethodDecl()) {
      // If we have an implementation in scope, check "private" methods.
      if (ObjCInterfaceDecl *ClassDecl = CurMeth->getClassInterface())
        if (ObjCImplementationDecl *ImpDecl = 
              ObjCImplementations[ClassDecl->getIdentifier()])
          Method = ImpDecl->getClassMethod(Sel);
      
      if (Method && DiagnoseUseOfDecl(Method, receiverLoc))
        return true;
    }
    if (!Method)
      Method = FactoryMethodPool[Sel].Method;
    if (!Method)
      Method = LookupInstanceMethodInGlobalPool(
                               Sel, SourceRange(lbrac,rbrac));
    if (CheckMessageArgumentTypes(ArgExprs, NumArgs, Sel, Method, false,
                                  lbrac, rbrac, returnType))
      return true;
    return new (Context) ObjCMessageExpr(RExpr, Sel, returnType, Method, lbrac,
                                         rbrac, ArgExprs, NumArgs);
  }
  
  ObjCMethodDecl *Method = 0;
  ObjCInterfaceDecl* ClassDecl = 0;
  
  // We allow sending a message to a qualified ID ("id<foo>"), which is ok as 
  // long as one of the protocols implements the selector (if not, warn).
  if (ObjCQualifiedIdType *QIT = dyn_cast<ObjCQualifiedIdType>(ReceiverCType)) {
    // Search protocols for instance methods.
    for (unsigned i = 0; i < QIT->getNumProtocols(); i++) {
      ObjCProtocolDecl *PDecl = QIT->getProtocols(i);
      if (PDecl && (Method = PDecl->lookupInstanceMethod(Sel)))
        break;
    }
    if (!Method)
      Diag(lbrac, diag::warn_method_not_found_in_protocol)
        << Sel << RExpr->getSourceRange();
  // Check for GCC extension "Class<foo>".
  } else if (ObjCQualifiedClassType *QIT = 
               dyn_cast<ObjCQualifiedClassType>(ReceiverCType)) {
    // Search protocols for class methods.
    for (unsigned i = 0; i < QIT->getNumProtocols(); i++) {
      ObjCProtocolDecl *PDecl = QIT->getProtocols(i);
      if (PDecl && (Method = PDecl->lookupClassMethod(Sel)))
        break;
    }
    if (!Method)
      Diag(lbrac, diag::warn_method_not_found_in_protocol)
        << Sel << RExpr->getSourceRange();
  } else if (const ObjCInterfaceType *OCIReceiver = 
                ReceiverCType->getAsPointerToObjCInterfaceType()) {
    // We allow sending a message to a pointer to an interface (an object).
    
    ClassDecl = OCIReceiver->getDecl();
    // FIXME: consider using LookupInstanceMethodInGlobalPool, since it will be
    // faster than the following method (which can do *many* linear searches). 
    // The idea is to add class info to InstanceMethodPool.
    Method = ClassDecl->lookupInstanceMethod(Sel);
    
    if (!Method) {
      // Search protocol qualifiers.
      for (ObjCQualifiedIdType::qual_iterator QI = OCIReceiver->qual_begin(),
           E = OCIReceiver->qual_end(); QI != E; ++QI) {
        if ((Method = (*QI)->lookupInstanceMethod(Sel)))
          break;
      }
    }
    
    if (!Method && !OCIReceiver->qual_empty())
      Diag(lbrac, diag::warn_method_not_found_in_protocol)
        << Sel << SourceRange(lbrac, rbrac);
    
    if (Method && DiagnoseUseOfDecl(Method, receiverLoc))
      return true;
  } else {
    Diag(lbrac, diag::error_bad_receiver_type)
      << RExpr->getType() << RExpr->getSourceRange();
    return true;
  }
  
  if (!Method) {
    // If we have an implementation in scope, check "private" methods.
    if (ClassDecl)
      if (ObjCImplementationDecl *ImpDecl = 
            ObjCImplementations[ClassDecl->getIdentifier()])
        Method = ImpDecl->getInstanceMethod(Sel);
        // If we still haven't found a method, look in the global pool. This
        // behavior isn't very desirable, however we need it for GCC
        // compatibility.
        if (!Method)
          Method = LookupInstanceMethodInGlobalPool(
                               Sel, SourceRange(lbrac,rbrac));
  }
  if (CheckMessageArgumentTypes(ArgExprs, NumArgs, Sel, Method, false,
                                lbrac, rbrac, returnType))
    return true;
  return new (Context) ObjCMessageExpr(RExpr, Sel, returnType, Method, lbrac,
                                       rbrac, ArgExprs, NumArgs);
}

//===----------------------------------------------------------------------===//
// ObjCQualifiedIdTypesAreCompatible - Compatibility testing for qualified id's.
//===----------------------------------------------------------------------===//

/// ProtocolCompatibleWithProtocol - return 'true' if 'lProto' is in the
/// inheritance hierarchy of 'rProto'.
static bool ProtocolCompatibleWithProtocol(ObjCProtocolDecl *lProto,
                                           ObjCProtocolDecl *rProto) {
  if (lProto == rProto)
    return true;
  for (ObjCProtocolDecl::protocol_iterator PI = rProto->protocol_begin(),
       E = rProto->protocol_end(); PI != E; ++PI)
    if (ProtocolCompatibleWithProtocol(lProto, *PI))
      return true;
  return false;
}

/// ClassImplementsProtocol - Checks that 'lProto' protocol
/// has been implemented in IDecl class, its super class or categories (if
/// lookupCategory is true). 
static bool ClassImplementsProtocol(ObjCProtocolDecl *lProto,
                                    ObjCInterfaceDecl *IDecl, 
                                    bool lookupCategory,
                                    bool RHSIsQualifiedID = false) {
  
  // 1st, look up the class.
  const ObjCList<ObjCProtocolDecl> &Protocols =
    IDecl->getReferencedProtocols();

  for (ObjCList<ObjCProtocolDecl>::iterator PI = Protocols.begin(),
       E = Protocols.end(); PI != E; ++PI) {
    if (ProtocolCompatibleWithProtocol(lProto, *PI))
      return true;
    // This is dubious and is added to be compatible with gcc.
    // In gcc, it is also allowed assigning a protocol-qualified 'id'
    // type to a LHS object when protocol in qualified LHS is in list
    // of protocols in the rhs 'id' object. This IMO, should be a bug.
    // FIXME: Treat this as an extension, and flag this as an error when
    //  GCC extensions are not enabled.
    if (RHSIsQualifiedID && ProtocolCompatibleWithProtocol(*PI, lProto))
      return true;
  }
  
  // 2nd, look up the category.
  if (lookupCategory)
    for (ObjCCategoryDecl *CDecl = IDecl->getCategoryList(); CDecl;
         CDecl = CDecl->getNextClassCategory()) {
      for (ObjCCategoryDecl::protocol_iterator PI = CDecl->protocol_begin(),
           E = CDecl->protocol_end(); PI != E; ++PI)
        if (ProtocolCompatibleWithProtocol(lProto, *PI))
          return true;
    }
  
  // 3rd, look up the super class(s)
  if (IDecl->getSuperClass())
    return 
      ClassImplementsProtocol(lProto, IDecl->getSuperClass(), lookupCategory,
                              RHSIsQualifiedID);
  
  return false;
}

/// ObjCQualifiedIdTypesAreCompatible - We know that one of lhs/rhs is an
/// ObjCQualifiedIDType.
bool Sema::ObjCQualifiedIdTypesAreCompatible(QualType lhs, QualType rhs,
                                             bool compare) {
  // Allow id<P..> and an 'id' or void* type in all cases.
  if (const PointerType *PT = lhs->getAsPointerType()) {
    QualType PointeeTy = PT->getPointeeType();
    if (Context.isObjCIdStructType(PointeeTy) || PointeeTy->isVoidType())
      return true;
  } else if (const PointerType *PT = rhs->getAsPointerType()) {
    QualType PointeeTy = PT->getPointeeType();
    if (Context.isObjCIdStructType(PointeeTy) || PointeeTy->isVoidType())
      return true;
  }
  
  if (const ObjCQualifiedIdType *lhsQID = lhs->getAsObjCQualifiedIdType()) {
    const ObjCQualifiedIdType *rhsQID = rhs->getAsObjCQualifiedIdType();
    const ObjCQualifiedInterfaceType *rhsQI = 0;
    QualType rtype;
    
    if (!rhsQID) {
      // Not comparing two ObjCQualifiedIdType's?
      if (!rhs->isPointerType()) return false;
      
      rtype = rhs->getAsPointerType()->getPointeeType();
      rhsQI = rtype->getAsObjCQualifiedInterfaceType();
      if (rhsQI == 0) {
        // If the RHS is a unqualified interface pointer "NSString*", 
        // make sure we check the class hierarchy.
        if (const ObjCInterfaceType *IT = rtype->getAsObjCInterfaceType()) {
          ObjCInterfaceDecl *rhsID = IT->getDecl();
          for (unsigned i = 0; i != lhsQID->getNumProtocols(); ++i) {
            // when comparing an id<P> on lhs with a static type on rhs,
            // see if static class implements all of id's protocols, directly or
            // through its super class and categories.
            if (!ClassImplementsProtocol(lhsQID->getProtocols(i), rhsID, true))
              return false;
          }
          return true;
        }
      }      
    }
    
    ObjCQualifiedIdType::qual_iterator RHSProtoI, RHSProtoE;
    if (rhsQI) { // We have a qualified interface (e.g. "NSObject<Proto> *").
      RHSProtoI = rhsQI->qual_begin();
      RHSProtoE = rhsQI->qual_end();
    } else if (rhsQID) { // We have a qualified id (e.g. "id<Proto> *").
      RHSProtoI = rhsQID->qual_begin();
      RHSProtoE = rhsQID->qual_end();
    } else {
      return false;
    }
    
    for (unsigned i =0; i < lhsQID->getNumProtocols(); i++) {
      ObjCProtocolDecl *lhsProto = lhsQID->getProtocols(i);
      bool match = false;

      // when comparing an id<P> on lhs with a static type on rhs,
      // see if static class implements all of id's protocols, directly or
      // through its super class and categories.
      for (; RHSProtoI != RHSProtoE; ++RHSProtoI) {
        ObjCProtocolDecl *rhsProto = *RHSProtoI;
        if (ProtocolCompatibleWithProtocol(lhsProto, rhsProto) ||
            (compare && ProtocolCompatibleWithProtocol(rhsProto, lhsProto))) {
          match = true;
          break;
        }
      }
      if (rhsQI) {
        // If the RHS is a qualified interface pointer "NSString<P>*", 
        // make sure we check the class hierarchy.
        if (const ObjCInterfaceType *IT = rtype->getAsObjCInterfaceType()) {
          ObjCInterfaceDecl *rhsID = IT->getDecl();
          for (unsigned i = 0; i != lhsQID->getNumProtocols(); ++i) {
            // when comparing an id<P> on lhs with a static type on rhs,
            // see if static class implements all of id's protocols, directly or
            // through its super class and categories.
            if (ClassImplementsProtocol(lhsQID->getProtocols(i), rhsID, true)) {
              match = true;
              break;
            }
          }
        }
      }
      if (!match)
        return false;
    }
    
    return true;
  }
  
  const ObjCQualifiedIdType *rhsQID = rhs->getAsObjCQualifiedIdType();
  assert(rhsQID && "One of the LHS/RHS should be id<x>");
    
  if (!lhs->isPointerType())
    return false;
  
  QualType ltype = lhs->getAsPointerType()->getPointeeType();
  if (const ObjCQualifiedInterfaceType *lhsQI =
         ltype->getAsObjCQualifiedInterfaceType()) {
    ObjCQualifiedIdType::qual_iterator LHSProtoI = lhsQI->qual_begin();
    ObjCQualifiedIdType::qual_iterator LHSProtoE = lhsQI->qual_end();
    for (; LHSProtoI != LHSProtoE; ++LHSProtoI) {
      bool match = false;
      ObjCProtocolDecl *lhsProto = *LHSProtoI;
      for (unsigned j = 0; j < rhsQID->getNumProtocols(); j++) {
        ObjCProtocolDecl *rhsProto = rhsQID->getProtocols(j);
        if (ProtocolCompatibleWithProtocol(lhsProto, rhsProto) ||
            (compare && ProtocolCompatibleWithProtocol(rhsProto, lhsProto))) {
          match = true;
          break;
        }
      }
      if (!match)
        return false;
    }
    return true;
  }
  
  if (const ObjCInterfaceType *IT = ltype->getAsObjCInterfaceType()) {
    // for static type vs. qualified 'id' type, check that class implements
    // all of 'id's protocols.
    ObjCInterfaceDecl *lhsID = IT->getDecl();
    for (unsigned j = 0; j < rhsQID->getNumProtocols(); j++) {
      ObjCProtocolDecl *rhsProto = rhsQID->getProtocols(j);
      if (!ClassImplementsProtocol(rhsProto, lhsID, compare, true))
        return false;
    }
    return true;
  }
  return false;
}

