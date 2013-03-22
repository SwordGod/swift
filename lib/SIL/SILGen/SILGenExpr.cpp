//===--- SILGenExpr.cpp - Implements Lowering of ASTs -> SIL for Exprs ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "swift/AST/AST.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/SIL/BBArgument.h"
#include "Initialization.h"
#include "LValue.h"
#include "ManagedValue.h"
#include "TypeLowering.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace Lowering;

namespace {
  class CleanupRValue : public Cleanup {
    Value rv;
  public:
    CleanupRValue(Value rv) : rv(rv) {}
    
    void emit(SILGenFunction &gen) override {
      gen.emitReleaseRValue(SILLocation(), rv);
    }
  };
  
  class CleanupTemporaryAllocation : public Cleanup {
    Value alloc;
  public:
    CleanupTemporaryAllocation(Value alloc) : alloc(alloc) {}
    
    void emit(SILGenFunction &gen) override {
      gen.B.createDeallocVar(SILLocation(), AllocKind::Stack, alloc);
    }
  };
  
  class CleanupMaterializedValue : public Cleanup {
    Value address;
  public:
    CleanupMaterializedValue(Value address) : address(address) {}
    
    void emit(SILGenFunction &gen) override {
      Value tmpValue = gen.B.createLoad(SILLocation(), address);
      gen.emitReleaseRValue(SILLocation(), tmpValue);
    }
  };
  
  class CleanupMaterializedAddressOnlyValue : public Cleanup {
    Value address;
  public:
    CleanupMaterializedAddressOnlyValue(Value address) : address(address) {}
    
    void emit(SILGenFunction &gen) override {
      gen.B.createDestroyAddr(SILLocation(), address);
    }
  };

  } // end anonymous namespace

ManagedValue SILGenFunction::emitManagedRValueWithCleanup(Value v) {
  if (v.getType().isAddress() ||
      getTypeLoweringInfo(v.getType().getSwiftType()).isTrivial()) {
    return ManagedValue(v);
  } else {
    Cleanups.pushCleanup<CleanupRValue>(v);
    return ManagedValue(v, getCleanupsDepth());
  }
}

ManagedValue SILGenFunction::emitManagedAddressOnlyValue(Value v) {
  Cleanups.pushCleanup<CleanupMaterializedAddressOnlyValue>(v);
  return ManagedValue(v, getCleanupsDepth(), /*isAddressOnlyValue=*/true);
}

static void initializeWithResult(SILGenFunction &gen,
                                 SILLocation loc,
                                 Initialization *I,
                                 ManagedValue result) {
  if (I->hasAddress()) {
    if (result.getType().isAddressOnly())
      result.forwardInto(gen, loc, I->getAddress(), /*isInitialize=*/true);
    else {
      gen.emitStore(loc, result, I->getAddress());
    }
    I->finishInitialization(gen);
    return;
  }
  
  // Destructure the tuple result into the subinitializations.
  // FIXME: address-only tuples
  // FIXME: visitTupleExpr and friends could destructure their subexpressions
  // directly into the subinitializations.
  ArrayRef<Initialization*> subInits = I->getSubInitializations();
  if (subInits.empty()) {
    // The initialization is a black hole or an empty tuple, in either case
    // we can do nothing and drop the result on the floor.
    return;
  }
  
  Value resultV = result.forward(gen);
  TupleType *tty = resultV.getType().getAs<TupleType>();
  
  assert(tty && "tuple initialization for non-tuple result?!");
  assert(tty->getFields().size() == subInits.size() &&
         "tuple initialization size does not match tuple size?!");
  for (size_t i = 0; i < subInits.size(); ++i) {
    SILType eltTy = gen.getLoweredLoadableType(tty->getFields()[i].getType());
    ManagedValue elt = ManagedValue(gen.B.createExtract(loc, resultV, i,
                                                        eltTy));
    initializeWithResult(gen, loc, subInits[i], elt);
  }
  I->finishInitialization(gen);
}

void SILGenFunction::emitExprInto(Expr *E, Initialization *I) {
  // FIXME: actually emit into the initialization. The initialization should
  // be passed down in the context argument to visit, and it should be the
  // visit*Expr method's responsibility to store to it if possible.
  ManagedValue result = visit(E);
  initializeWithResult(*this, E, I, result);
}

ManagedValue SILGenFunction::visit(swift::Expr *E) {
  return visit(E, SGFContext());
}

ManagedValue SILGenFunction::visitApplyExpr(ApplyExpr *E, SGFContext C) {
  return emitApplyExpr(E);
}

Value SILGenFunction::emitGlobalConstantRef(SILLocation loc,
                                            SILConstant constant) {
  return B.createConstantRef(loc, constant, SGM.getConstantType(constant));
}

Value SILGenFunction::emitUnmanagedConstantRef(SILLocation loc,
                                               SILConstant constant) {
  // If this is a reference to a local constant, grab it.
  if (LocalConstants.count(constant)) {
    return LocalConstants[constant];
  }
  
  // Otherwise, use a global ConstantRefInst.
  return emitGlobalConstantRef(loc, constant);
}

ManagedValue SILGenFunction::emitConstantRef(SILLocation loc,
                                             SILConstant constant) {
  // If this is a reference to a local constant, grab it.
  if (LocalConstants.count(constant)) {
    Value v = LocalConstants[constant];
    emitRetainRValue(loc, v);
    return emitManagedRValueWithCleanup(v);
  }
  
  // Otherwise, use a global ConstantRefInst.
  Value c = emitGlobalConstantRef(loc, constant);
  return ManagedValue(c);
}

ManagedValue SILGenFunction::emitReferenceToDecl(SILLocation loc,
                                                 ValueDecl *decl) {
  // If this is a reference to a type, produce a metatype.
  if (isa<TypeDecl>(decl)) {
    assert(decl->getType()->is<MetaTypeType>() &&
           "type declref does not have metatype type?!");
    return ManagedValue(B.createMetatype(loc, getLoweredType(decl->getType())));
  }
  
  // If this is a reference to a var, produce an address.
  if (VarDecl *var = dyn_cast<VarDecl>(decl)) {
    // If it's a property, invoke its getter.
    if (var->isProperty()) {
      ManagedValue get = emitConstantRef(loc,
                                  SILConstant(decl, SILConstant::Kind::Getter));
      return ManagedValue(emitGetProperty(loc, get, Nothing, Nothing).address);
    }
    
    // For local decls, use the address we allocated.
    if (VarLocs.count(decl)) {
      return ManagedValue(VarLocs[decl].address);
    }
    assert(!decl->getDeclContext()->isLocalContext() &&
           "no location for local var!");
    // If this is a global variable, invoke its accessor function to get its
    // address.
    Value accessor = emitGlobalConstantRef(loc, SILConstant(decl));
    return emitApply(loc, accessor, {});
  }
  
  // If the referenced decl isn't a VarDecl, it should be a constant of some
  // sort.
  assert(!decl->getTypeOfReference()->is<LValueType>() &&
         "unexpected lvalue decl ref?!");

  return emitConstantRef(loc, SILConstant(decl));
}

ManagedValue SILGenFunction::visitDeclRefExpr(DeclRefExpr *E, SGFContext C) {
  return emitReferenceToDecl(E, E->getDecl());
}

ManagedValue SILGenFunction::visitSuperRefExpr(SuperRefExpr *E, SGFContext C) {
  return emitReferenceToDecl(E, E->getThis());
}

ManagedValue SILGenFunction::visitOtherConstructorDeclRefExpr(
                                OtherConstructorDeclRefExpr *E, SGFContext C) {
  llvm_unreachable("unapplied reference to constructor?!");
}

ManagedValue SILGenFunction::visitIntegerLiteralExpr(IntegerLiteralExpr *E,
                                                     SGFContext C) {
  return ManagedValue(B.createIntegerLiteral(E));
}
ManagedValue SILGenFunction::visitFloatLiteralExpr(FloatLiteralExpr *E,
                                                   SGFContext C) {
  return ManagedValue(B.createFloatLiteral(E));
}
ManagedValue SILGenFunction::visitCharacterLiteralExpr(CharacterLiteralExpr *E,
                                                       SGFContext C)
{
  return ManagedValue(B.createIntegerLiteral(E));
}
ManagedValue SILGenFunction::visitStringLiteralExpr(StringLiteralExpr *E,
                                                    SGFContext C) {
  return ManagedValue(B.createStringLiteral(E));
}

ManagedValue SILGenFunction::visitLoadExpr(LoadExpr *E, SGFContext C) {
  ManagedValue SubV = visit(E->getSubExpr());
  TypeLoweringInfo const &ti = getTypeLoweringInfo(E->getType());
  if (ti.isAddressOnly()) {
    // We can't load address-only types. Just pass on the address.
    return ManagedValue(SubV.getUnmanagedValue(),
                        /*isAddressOnlyValue=*/ true);
  }
  Value loadedV = B.createLoad(E, SubV.getUnmanagedValue());
  emitRetainRValue(E, loadedV);
  return emitManagedRValueWithCleanup(loadedV);
}

Value SILGenFunction::emitTemporaryAllocation(SILLocation loc,
                                              SILType ty) {
  Value tmpMem = B.createAllocVar(loc, AllocKind::Stack, ty);
  Cleanups.pushCleanup<CleanupTemporaryAllocation>(tmpMem);
  return tmpMem;
}

Materialize SILGenFunction::emitMaterialize(SILLocation loc, ManagedValue v) {
  // Address-only values are already materialized.
  if (v.isAddressOnlyValue()) {
    return Materialize{v.getValue(), v.getCleanup(),
                       /*isAddressOnlyValue=*/ true};
  }
  
  assert(!v.getType().isAddress() &&
         "can't materialize a reference");
  
  Value tmpMem = emitTemporaryAllocation(loc, v.getType());
  emitStore(loc, v, tmpMem);
  
  CleanupsDepth valueCleanup = CleanupsDepth::invalid();
  if (!getTypeLoweringInfo(v.getType().getSwiftType()).isTrivial()) {
    Cleanups.pushCleanup<CleanupMaterializedValue>(tmpMem);
    valueCleanup = getCleanupsDepth();
  }
  
  return Materialize{tmpMem, valueCleanup,
                     /*isAddressOnlyValue=*/ false};
}

ManagedValue Materialize::consume(SILGenFunction &gen, SILLocation loc) {
  assert(!isAddressOnlyValue &&
         "address-only value must be consumed with consumeInto");
  if (valueCleanup.isValid())
    gen.Cleanups.setCleanupState(valueCleanup, CleanupState::Dead);
  return gen.emitManagedRValueWithCleanup(gen.B.createLoad(loc, address));
}

ManagedValue SILGenFunction::visitMaterializeExpr(MaterializeExpr *E,
                                                  SGFContext C) {
  // Evaluate the value, then use it to initialize a new temporary and return
  // the temp's address.
  ManagedValue V = visit(E->getSubExpr());
  return ManagedValue(emitMaterialize(E, V).address);
}

ManagedValue SILGenFunction::visitDerivedToBaseExpr(DerivedToBaseExpr *E,
                                                    SGFContext C) {
  ManagedValue original = visit(E->getSubExpr());
  Value converted = B.createUpcast(E,
                                   original.getValue(),
                                   getLoweredType(E->getType()));
  return ManagedValue(converted, original.getCleanup());
}

ManagedValue SILGenFunction::visitMetatypeConversionExpr(
                                                   MetatypeConversionExpr *E,
                                                   SGFContext C) {
  ManagedValue metaBase = visit(E->getSubExpr());
  return ManagedValue(B.createUpcast(E, metaBase.getUnmanagedValue(),
                                     getLoweredLoadableType(E->getType())));
}

ManagedValue SILGenFunction::visitArchetypeToSuperExpr(
                                               swift::ArchetypeToSuperExpr *E,
                                               SGFContext C) {
  ManagedValue archetype = visit(E->getSubExpr());
  Value base = B.createArchetypeToSuper(E,
                                        archetype.getValue(),
                                        getLoweredLoadableType(E->getType()));
  if (archetype.hasCleanup()) {
    // If an archetype has a base class, then it must have been of some
    // reference type, so releasing the resulting class-type value is equivalent
    // to destroying the archetype. We can thus "take" the archetype and disable
    // its destructor cleanup if it was a temporary.
    Cleanups.setCleanupState(archetype.getCleanup(), CleanupState::Dead);
  } else {
    // The archetype isn't a temporary, so we need our own retain.
    emitRetainRValue(E, base);
  }
  return emitManagedRValueWithCleanup(base);
}

static ManagedValue emitImplicitConvert(SILGenFunction &gen,
                                        ImplicitConversionExpr *e) {
  ManagedValue original = gen.visit(e->getSubExpr());
  Value converted = gen.B.createImplicitConvert(e,
                                                original.getValue(),
                                                gen.getLoweredType(e->getType()));
  return ManagedValue(converted, original.getCleanup());
}

ManagedValue SILGenFunction::visitRequalifyExpr(RequalifyExpr *E,
                                                SGFContext C) {
  if (E->getType()->is<LValueType>()) {
    // Ignore lvalue qualifiers.
    return visit(E->getSubExpr());
  }
  return emitImplicitConvert(*this, E);
}

ManagedValue SILGenFunction::visitFunctionConversionExpr(
                                                      FunctionConversionExpr *E,
                                                      SGFContext C)
{
  return emitImplicitConvert(*this, E);
}

ManagedValue SILGenFunction::visitErasureExpr(ErasureExpr *E, SGFContext C) {
  ManagedValue concrete = visit(E->getSubExpr());
  // Allocate the existential.
  Value existential = emitTemporaryAllocation(E, getLoweredType(E->getType()));

  if (concrete.getType().isExistentialType()) {
    // If the source value is already of a protocol type, we can use
    // upcast_existential to steal its already-initialized witness tables and
    // concrete value.

    B.createUpcastExistential(E, concrete.getValue(), existential,
                              /*isTake=*/concrete.hasCleanup(),
                              E->getConformances());
  } else {
    // Otherwise, we need to initialize a new existential container from
    // scratch.
    
    // FIXME: Need to stage cleanups better here. If code fails between
    // InitExistential and initializing the value, clean up using
    // DeinitExistential.
    
    // Allocate the concrete value inside the container.
    Value valueAddr = B.createInitExistential(E, existential,
                                              concrete.getType(),
                                              E->getConformances());
    // Initialize the concrete value.
    if (concrete.getType().isAddressOnly()) {
      concrete.forwardInto(*this, E, valueAddr,
                           /*isInitialize=*/true);
    } else {
      emitStore(E, concrete, valueAddr);
    }
  }
  
  return emitManagedAddressOnlyValue(existential);
}

ManagedValue SILGenFunction::visitCoerceExpr(CoerceExpr *E, SGFContext C) {
  // FIXME: do something with lhs value?
  visit(E->getLHS());
  return ManagedValue(B.createCoerce(E, visit(E->getRHS()).getValue(),
                                     getLoweredType(E->getType())));
}

ManagedValue SILGenFunction::visitDowncastExpr(DowncastExpr *E, SGFContext C) {
  // FIXME: do something with lhs value?
  visit(E->getLHS());
  ManagedValue original = visit(E->getRHS());
  Value converted = B.createDowncast(E, original.getValue(),
                                     getLoweredLoadableType(E->getType()));
  return ManagedValue(converted, original.getCleanup());
}

ManagedValue SILGenFunction::visitSuperToArchetypeExpr(SuperToArchetypeExpr *E,
                                                       SGFContext C) {
  visit(E->getLHS());
  ManagedValue base = visit(E->getRHS());
  // Allocate an archetype to hold the downcast value.
  Value archetype = emitTemporaryAllocation(E, getLoweredType(E->getType()));
  // Initialize it with a SuperToArchetypeInst.
  B.createSuperToArchetype(E, base.forward(*this), archetype);
  
  return emitManagedAddressOnlyValue(archetype);
}

ManagedValue SILGenFunction::visitParenExpr(ParenExpr *E, SGFContext C) {
  return visit(E->getSubExpr(), C);
}

static ManagedValue emitVarargs(SILGenFunction &gen,
                                SILLocation loc,
                                Type baseTy,
                                ArrayRef<Value> elements,
                                Expr *VarargsInjectionFn) {
  Value numEltsVal = gen.B.createIntegerValueInst(elements.size(),
                      SILType::getBuiltinIntegerType(64, gen.F.getContext()));
  AllocArrayInst *allocArray = gen.B.createAllocArray(loc,
                                                  gen.getLoweredType(baseTy),
                                                  numEltsVal);
  
  Value objectPtr(allocArray, 0);
  Value basePtr(allocArray, 1);

  for (size_t i = 0, size = elements.size(); i < size; ++i) {
    Value eltPtr = i == 0
      ? basePtr
      : gen.B.createIndexAddr(loc, basePtr, i);
    gen.emitStore(loc, ManagedValue(elements[i]), eltPtr);
  }

  return gen.emitArrayInjectionCall(objectPtr, basePtr,
                                    numEltsVal, VarargsInjectionFn);
}

ManagedValue SILGenFunction::emitTuple(Expr *E,
                                       ArrayRef<Expr *> Elements,
                                       Type VarargsBaseTy,
                                       ArrayRef<Value> VariadicElements,
                                       Expr *VarargsInjectionFunction,
                                       SGFContext C) {  
  if (SmallVectorImpl<Value> *argsV = C.getArgumentVector()) {
    // If we're in a function argument context, destructure our subexpressions
    // directly onto the function call's argument vector.
    for (auto &elt : Elements) {
      emitApplyArguments(elt, *argsV);
    }
    
    if (VarargsBaseTy)
      argsV->push_back(emitVarargs(*this, E,
                                   VarargsBaseTy,
                                   VariadicElements, VarargsInjectionFunction)
                         .forward(*this));
    
    return ManagedValue();
  }
  
  // Otherwise, create a tuple value.
  // FIXME: address-only tuples
  llvm::SmallVector<Value, 8> tupleV;
  for (auto &elt : Elements)
    tupleV.push_back(visit(elt).forward(*this));

  if (VarargsBaseTy)
    tupleV.push_back(emitVarargs(*this, E,
                                 VarargsBaseTy,
                                 VariadicElements, VarargsInjectionFunction)
                       .forward(*this));
  
  // FIXME: address-only tuple
  return emitManagedRValueWithCleanup(B.createTuple(E,
                                          getLoweredLoadableType(E->getType()),
                                          tupleV));
}

ManagedValue SILGenFunction::visitTupleExpr(TupleExpr *E, SGFContext C) {
  return emitTuple(E, E->getElements(),
                   Type(), /*VariadicElements=*/ {}, /*InjectionFn=*/ nullptr,
                   C);
}

ManagedValue SILGenFunction::visitSpecializeExpr(SpecializeExpr *E,
                                                 SGFContext C) {
  return ManagedValue(B.createSpecialize(
                    E,
                    visit(E->getSubExpr()).getUnmanagedValue(),
                    E->getSubstitutions(),
                    getLoweredLoadableType(getThinFunctionType(E->getType()))));
}

ManagedValue SILGenFunction::visitAddressOfExpr(AddressOfExpr *E,
                                                SGFContext C) {
  return visit(E->getSubExpr(), C);
}

namespace {

static ManagedValue emitExtractLoadableElement(SILGenFunction &gen,
                                               Expr *e, ManagedValue base,
                                               unsigned elt) {
  assert(!base.getType().hasReferenceSemantics() &&
         "can't extract from reference types here");
  if (e->getType()->is<LValueType>()) {
    // Get the element address relative to the aggregate's address.
    Value address;
    assert(base.getType().isAddress() &&
           "base of lvalue member ref must be ref type or address");
    address = gen.B.createElementAddr(e,
                                    base.getUnmanagedValue(),
                                    elt,
                                    gen.getLoweredType(e->getType()));
    return ManagedValue(address);
  } else {
    // Extract the element from the original aggregate value.
    Value extract = gen.B.createExtract(e,
                                        base.getValue(),
                                        elt,
                                        gen.getLoweredType(e->getType()));
    
    gen.emitRetainRValue(e, extract);
    return gen.emitManagedRValueWithCleanup(extract);
  }
}
  
static ManagedValue emitExtractFromClass(SILGenFunction &gen,
                                         Expr *e, ManagedValue base,
                                         VarDecl *field) {
  return ManagedValue(gen.B.createRefElementAddr(e,
                                             base.getValue(),
                                             field,
                                             gen.getLoweredType(e->getType())));
}

template<typename ANY_MEMBER_REF_EXPR>
ManagedValue emitAnyMemberRefExpr(SILGenFunction &gen,
                                  ANY_MEMBER_REF_EXPR *e,
                                  ArrayRef<Substitution> substitutions) {
  SILType ty = gen.getLoweredType(e->getBase()->getType()->getRValueType());
  
  // If this is a physical field, derive its address directly.
  if (VarDecl *var = dyn_cast<VarDecl>(e->getDecl())) {
    if (!var->isProperty()) {
      SILCompoundTypeInfo *cti = gen.SGM.M.getCompoundTypeInfo(ty);
      // We can get to the element directly with element_addr or
      // ref_element_addr.
      ManagedValue base = gen.visit(e->getBase());
      if (ty.hasReferenceSemantics())
        return emitExtractFromClass(gen, e, base, var);
      size_t index = cti->getIndexOfMemberDecl(var);
      return emitExtractLoadableElement(gen, e, base, index);
    }
  }

  // Otherwise, call the getter.
  Type baseTy = e->getBase()->getType();
  (void)baseTy;
  // We have to load the element indirectly through a property.
  assert((baseTy->is<LValueType>() || baseTy->hasReferenceSemantics()) &&
         "member ref of a non-lvalue?!");
  ManagedValue base = gen.visit(e->getBase());
  // Get the getter function, which will have type This -> () -> T.
  // The type will be a polymorphic function if the This type is generic.
  ManagedValue getter = gen.emitSpecializedPropertyConstantRef(e,
                                        e->getBase(),
                                        /*subscriptExpr=*/nullptr,
                                        SILConstant(e->getDecl(),
                                                    SILConstant::Kind::Getter),
                                        substitutions);
  // Apply the getter.
  Materialize propTemp = gen.emitGetProperty(e, getter, base, Nothing);
  return ManagedValue(propTemp.address);
}

} // end anonymous namespace

ManagedValue SILGenFunction::emitSpecializedPropertyConstantRef(
                                          Expr *expr, Expr *baseExpr,
                                          Expr /*nullable*/ *subscriptExpr,
                                          SILConstant constant,
                                          ArrayRef<Substitution> substitutions)
{
  // Get the accessor function. The type will be a polymorphic function if
  // the This type is generic.
  ManagedValue method(emitGlobalConstantRef(expr, constant));
  // If there are substitutions, specialize the generic getter.
  if (!substitutions.empty()) {
    assert(method.getType().is<PolymorphicFunctionType>() &&
           "generic getter is not of a poly function type");
    TypeConverter &tc = SGM.Types;
    Type propType;
    if (subscriptExpr) {
      propType = tc.getSubscriptPropertyType(constant.kind,
                                            subscriptExpr->getType(),
                                            expr->getType()->getRValueType());
    } else {
      propType = tc.getPropertyType(constant.kind,
                                   expr->getType()->getRValueType());
    }
    propType = tc.getMethodTypeInContext(baseExpr->getType()->getRValueType(),
                                        propType);
    SILType lPropType = getLoweredLoadableType(getThinFunctionType(propType),
                                               method.getType().getUncurryLevel());
    method = ManagedValue(B.createSpecialize(expr, method.getUnmanagedValue(),
                                             substitutions, lPropType));
  }
  assert(method.getType().is<FunctionType>() &&
         "getter is not of a concrete function type");
  return method;
}

ManagedValue SILGenFunction::emitMethodRef(SILLocation loc,
                                           Value thisValue,
                                           SILConstant methodConstant) {
  SILType methodType = SGM.getConstantType(methodConstant);
  Value methodValue = B.createConstantRef(loc,
                                          methodConstant,
                                          methodType);
  /// If the 'this' type is a bound generic, specialize the method ref with
  /// its substitutions
  /// FIXME: Doesn't specialize generic method archetypes.
  if (BoundGenericType *bgt = thisValue.getType().getAs<BoundGenericType>()) {
    PolymorphicFunctionType *methodFT =
      methodType.castTo<PolymorphicFunctionType>();
    Type specializedType = FunctionType::get(thisValue.getType().getSwiftType(),
                                             methodFT->getResult(),
                                             /*isAutoClosure*/ false,
                                             /*isBlock*/ false,
                                             /*isThin*/ true,
                                             F.getContext());
    methodValue = B.createSpecialize(loc, methodValue, bgt->getSubstitutions(),
               getLoweredLoadableType(specializedType,
                                      methodValue.getType().getUncurryLevel()));
    return emitManagedRValueWithCleanup(methodValue);
  }
  
  return ManagedValue(methodValue);
}

ManagedValue SILGenFunction::visitMemberRefExpr(MemberRefExpr *E,
                                                SGFContext C) {
  return emitAnyMemberRefExpr(*this, E, {});
}

ManagedValue SILGenFunction::visitGenericMemberRefExpr(GenericMemberRefExpr *E,
                                                       SGFContext C)
{
  if (E->getBase()->getType()->is<MetaTypeType>()) {
    assert(E->getType()->is<MetaTypeType>() &&
           "generic_member_ref of metatype should give metatype");
    // If the base and member are metatypes, emit an associated_metatype inst
    // to extract the associated type from the type metadata.
    Value baseMetatype = visit(E->getBase()).getUnmanagedValue();
    return ManagedValue(B.createAssociatedMetatype(E,
                             baseMetatype,
                             getLoweredLoadableType(E->getType())));

  }
  return emitAnyMemberRefExpr(*this, E, E->getSubstitutions());
}

Value SILGenFunction::emitArchetypeMethod(ArchetypeMemberRefExpr *e,
                                          Value archetype) {
  if (isa<FuncDecl>(e->getDecl())) {
    // This is a method reference. Extract the method implementation from the
    // archetype and apply the "this" argument.
    Type methodType = FunctionType::get(archetype.getType().getSwiftType(),
                                        e->getType(),
                                        /*isAutoClosure*/ false,
                                        /*isBlock*/ false,
                                        /*isThin*/ true,
                                        F.getContext());
    SILConstant c(e->getDecl());
    return B.createArchetypeMethod(e, archetype,
                         c, getLoweredLoadableType(methodType, c.uncurryLevel));
  } else {
    llvm_unreachable("archetype properties not yet implemented");
  }
}

ManagedValue SILGenFunction::visitArchetypeMemberRefExpr(
                                                    ArchetypeMemberRefExpr *E,
                                                    SGFContext C) {
  Value archetype = visit(E->getBase()).getUnmanagedValue();
  assert((archetype.getType().isAddress() ||
          archetype.getType().is<MetaTypeType>()) &&
         "archetype must be an address or metatype");
  Value method = emitArchetypeMethod(E, archetype);
  // FIXME: curried archetype
  return emitApply(E, method, archetype);
}

Value SILGenFunction::emitProtocolMethod(ExistentialMemberRefExpr *e,
                                         Value existential) {
  if (isa<FuncDecl>(e->getDecl())) {
    // This is a method reference. Extract the method implementation from the
    // archetype and apply the "this" argument.
    Type methodType = FunctionType::get(F.getContext().TheOpaquePointerType,
                                        e->getType(),
                                        F.getContext());
    SILConstant c(e->getDecl());
    return B.createProtocolMethod(e, existential,
                        c, getLoweredLoadableType(methodType, c.uncurryLevel));
  } else {
    llvm_unreachable("existential properties not yet implemented");
  }
}

ManagedValue SILGenFunction::visitExistentialMemberRefExpr(
                                                 ExistentialMemberRefExpr *E,
                                                 SGFContext C) {
  Value existential = visit(E->getBase()).getUnmanagedValue();
  assert(existential.getType().isAddress() &&
         "existential must be an address");
  Value projection = B.createProjectExistential(E, existential);
  Value method = emitProtocolMethod(E, existential);
  // FIXME: curried existential
  return emitApply(E, method, projection);
}

ManagedValue SILGenFunction::visitDotSyntaxBaseIgnoredExpr(
                                                  DotSyntaxBaseIgnoredExpr *E,
                                                  SGFContext C) {
  visit(E->getLHS());
  return visit(E->getRHS());
}

ManagedValue SILGenFunction::visitModuleExpr(ModuleExpr *E, SGFContext C) {
  // FIXME: modules are currently empty types. if we end up having module
  // metatypes this will need to change.
  return ManagedValue(B.createModule(E, getLoweredLoadableType(E->getType())));
}

namespace {

template<typename ANY_SUBSCRIPT_EXPR>
ManagedValue emitAnySubscriptExpr(SILGenFunction &gen,
                                  ANY_SUBSCRIPT_EXPR *e,
                                  ArrayRef<Substitution> substitutions)
{
  SubscriptDecl *sd = e->getDecl();
  ManagedValue base = gen.visit(e->getBase());
  llvm::SmallVector<Value, 2> indexArgs;
  
  gen.emitApplyArguments(e->getIndex(), indexArgs);
  
  // Get the getter function, which will have type This -> Index -> () -> T.
  ManagedValue getter = gen.emitSpecializedPropertyConstantRef(
                                    e, e->getBase(), e->getIndex(),
                                    SILConstant(sd, SILConstant::Kind::Getter),
                                    substitutions);
  // Apply the getter.
  Materialize propTemp = gen.emitGetProperty(e, getter, base, indexArgs);
  return ManagedValue(propTemp.address);
}

}

ManagedValue SILGenFunction::visitSubscriptExpr(SubscriptExpr *E,
                                                SGFContext C) {
  return emitAnySubscriptExpr(*this, E, {});
}

ManagedValue SILGenFunction::visitGenericSubscriptExpr(GenericSubscriptExpr *E,
                                                       SGFContext C)
{
  return emitAnySubscriptExpr(*this, E, E->getSubstitutions());
}

ManagedValue SILGenFunction::visitTupleElementExpr(TupleElementExpr *E,
                                                   SGFContext C) {
  // FIXME: address-only tuples
  return emitExtractLoadableElement(*this, E, visit(E->getBase()),
                                    E->getFieldNumber());
}

ManagedValue SILGenFunction::emitTupleShuffleOfExprs(Expr *E,
                                             ArrayRef<Expr *> InExprs,
                                             ArrayRef<int> ElementMapping,
                                             Expr *VarargsInjectionFunction,
                                             SGFContext C) {
  // Collect the shuffled subexprs.
  SmallVector<Expr *, 8> ResultElements;
  SmallVector<Value, 4> variadicValues;
  Type variadicBaseTy;
  
  // Loop over each result element to compute it.
  ArrayRef<TupleTypeElt> outerFields =
  E->getType()->castTo<TupleType>()->getFields();
  
  auto shuffleIndexIterator = ElementMapping.begin();
  for (const TupleTypeElt &outerField : outerFields) {
    int shuffleIndex = *shuffleIndexIterator++;
    
    // If the shuffle index is DefaultInitialize, we're supposed to use the
    // default value.
    if (shuffleIndex == TupleShuffleExpr::DefaultInitialize) {
      assert(outerField.hasInit() && "no default initializer for field!");
      ResultElements.push_back(outerField.getInit()->getExpr());
      continue;
    }
    
    // If the shuffle index is FirstVariadic, it is the beginning of the list of
    // varargs inputs.  Save this case for last.
    if (shuffleIndex != TupleShuffleExpr::FirstVariadic) {
      // Map from a different tuple element.
      ResultElements.push_back(InExprs[shuffleIndex]);
      continue;
    }
    
    assert(outerField.isVararg() && "Cannot initialize nonvariadic element");
    
    // Okay, we have a varargs tuple element.  All the remaining elements feed
    // into the varargs portion of this, which is then constructed into a Slice
    // through an informal protocol captured by the InjectionFn in the
    // TupleShuffleExpr.
    assert(VarargsInjectionFunction &&
           "no injection function for varargs tuple?!");
    auto shuffleIndexIteratorEnd = ElementMapping.end();
    
    while (shuffleIndexIterator != shuffleIndexIteratorEnd) {
      unsigned sourceField = *shuffleIndexIterator++;
      // Variadic values are never destructured so we greedily evaluate our
      // subexpressions here. There's nothing to gain from passing the exprs
      // down further.
      variadicValues.push_back(visit(InExprs[sourceField]).forward(*this));
    }

    variadicBaseTy = outerField.getVarargBaseTy();
    break;
  }
  
  return emitTuple(E, ResultElements,
                   variadicBaseTy, variadicValues, VarargsInjectionFunction,
                   C);
}

ManagedValue SILGenFunction::emitTupleShuffle(Expr *E,
                                      ArrayRef<Value> InOps,
                                      ArrayRef<int> ElementMapping,
                                      Expr *VarargsInjectionFunction) {
  // Collect the new elements.
  SmallVector<Value, 8> ResultElements;

  // Loop over each result element to compute it.
  ArrayRef<TupleTypeElt> outerFields =
    E->getType()->castTo<TupleType>()->getFields();

  auto shuffleIndexIterator = ElementMapping.begin();
  for (const TupleTypeElt &outerField : outerFields) {
    int shuffleIndex = *shuffleIndexIterator++;

    // If the shuffle index is DefaultInitialize, we're supposed to use the
    // default value.
    if (shuffleIndex == TupleShuffleExpr::DefaultInitialize) {
      assert(outerField.hasInit() && "no default initializer for field!");
      ResultElements.push_back(visit(outerField.getInit()->getExpr())
                                 .forward(*this));
      continue;
    }

    // If the shuffle index is FirstVariadic, it is the beginning of the list of
    // varargs inputs.  Save this case for last.
    if (shuffleIndex != TupleShuffleExpr::FirstVariadic) {
      // Map from a different tuple element.
      ResultElements.push_back(InOps[shuffleIndex]);
      continue;
    }

    assert(outerField.isVararg() && "Cannot initialize nonvariadic element");

    // Okay, we have a varargs tuple element.  All the remaining elements feed
    // into the varargs portion of this, which is then constructed into a Slice
    // through an informal protocol captured by the InjectionFn in the
    // TupleShuffleExpr.
    assert(VarargsInjectionFunction &&
           "no injection function for varargs tuple?!");
    auto shuffleIndexIteratorEnd = ElementMapping.end();
    SmallVector<Value, 4> variadicValues;
    
    while (shuffleIndexIterator != shuffleIndexIteratorEnd) {
      unsigned sourceField = *shuffleIndexIterator++;
      variadicValues.push_back(InOps[sourceField]);
    }
    
    ResultElements.push_back(emitVarargs(*this,
                                         E, outerField.getVarargBaseTy(),
                                         variadicValues,
                                         VarargsInjectionFunction)
                              .forward(*this));
    break;
  }

  // FIXME address-only tuples
  return emitManagedRValueWithCleanup(B.createTuple(E,
                                          getLoweredLoadableType(E->getType()),
                                          ResultElements));
}

ManagedValue SILGenFunction::visitTupleShuffleExpr(TupleShuffleExpr *E,
                                                   SGFContext C) {
  // If our subexpr is a literal TupleExpr, then shuffle the literal exprs
  // directly.
  Expr *subExpr = E->getSubExpr();
  while (ParenExpr *pe = dyn_cast<ParenExpr>(subExpr)) {
    subExpr = pe->getSubExpr();
  }
  if (TupleExpr *tupleE = dyn_cast<TupleExpr>(subExpr)) {
    return emitTupleShuffleOfExprs(E, tupleE->getElements(),
                                   E->getElementMapping(),
                                   E->getVarargsInjectionFunctionOrNull(),
                                   C);
  }
  
  // Otherwise, TupleShuffle expands out to extracts+inserts. Start by emitting
  // and exploding the base expression that we'll shuffle.
  Value Op = visit(E->getSubExpr()).getValue();
  SmallVector<Value, 8> InElts;
  unsigned EltNo = 0;
  for (auto &InField : Op.getType().castTo<TupleType>()->getFields()) {
    // FIXME address-only tuples
    Value elt = B.createExtract(SILLocation(), Op, EltNo++,
                                getLoweredLoadableType(InField.getType()));
    emitRetainRValue(E, elt);
    InElts.push_back(elt);
  }

  return emitTupleShuffle(E, InElts, E->getElementMapping(),
                          E->getVarargsInjectionFunctionOrNull());
}

ManagedValue SILGenFunction::visitScalarToTupleExpr(ScalarToTupleExpr *E,
                                                    SGFContext C) {
  // Collect the expressions to construct the new elements.
  SmallVector<Expr *, 2> ResultElements;
  SmallVector<Value, 2> VariadicElements;
  Type VariadicBaseTy;
  
  // Perform a shuffle to create a tuple around the scalar along with any
  // default values or varargs.
  auto outerFields = E->getType()->castTo<TupleType>()->getFields();
  bool didEmitScalarField = false;
  for (unsigned i = 0, e = outerFields.size(); i != e; ++i) {
    // Handle the variadic argument. If we didn't emit the scalar field yet,
    // it goes into the variadic array; otherwise, the variadic array is empty.
    if (outerFields[i].isVararg()) {
      assert(i == e - 1 && "vararg isn't last?!");
      if (!didEmitScalarField)
        VariadicElements.push_back(visit(E->getSubExpr()).forward(*this));
      VariadicBaseTy = outerFields[i].getVarargBaseTy();
      break;
    }

    // Emit the subexpression into the scalar field indicated by the AST node.
    else if (i == E->getScalarField()) {
      ResultElements.push_back(E->getSubExpr());
      didEmitScalarField = true;
    }
    // Fill in the other fields with their default initializers.
    else {
      assert(outerFields[i].hasInit() &&
             "no default initializer in non-scalar field of scalar-to-tuple?!");
      ResultElements.push_back(outerFields[i].getInit()->getExpr());
    }
  }

  return emitTuple(E, ResultElements,
                   VariadicBaseTy, VariadicElements,
                   E->getVarargsInjectionFunction(),
                   C);
}

ManagedValue SILGenFunction::visitNewArrayExpr(NewArrayExpr *E, SGFContext C) {
  Value NumElements = visit(E->getBounds()[0].Value).getValue();

  // Allocate the array.
  AllocArrayInst *AllocArray = B.createAllocArray(E,
                                            getLoweredType(E->getElementType()),
                                            NumElements);

  Value ObjectPtr(AllocArray, 0), BasePtr(AllocArray, 1);

  // FIXME: We need to initialize the elements of the array that are now
  // allocated.

  // Finally, build and return a Slice instance using the object
  // header/base/count.
  return emitArrayInjectionCall(ObjectPtr, BasePtr, NumElements,
                                E->getInjectionFunction());
}

ManagedValue SILGenFunction::visitMetatypeExpr(MetatypeExpr *E, SGFContext C) {
  // Evaluate the base if present.
  if (E->getBase()) {
    ManagedValue base = visit(E->getBase());
    // For class types, look up the dynamic metatype.
    if (base.getType().getSwiftType()->getClassOrBoundGenericClass()) {
      return ManagedValue(
        B.createClassMetatype(E, getLoweredLoadableType(E->getType()),
                              base.getValue()));
    }
    // FIXME: Archetypes and protocol types should need dynamic metatype lookup
    // too.
  }
  
  // Otherwise, ignore the base and return the static metatype.
  return ManagedValue(
                    B.createMetatype(E, getLoweredLoadableType(E->getType())));
}

ManagedValue SILGenFunction::emitClosureForCapturingExpr(SILLocation loc,
                                                         SILConstant constant,
                                                         CapturingExpr *body) {
  // FIXME: Stash the capture args somewhere and curry them on demand rather
  // than here.
  assert(constant.uncurryLevel == 1 &&
         "curried local functions not yet supported");

  auto captures = body->getCaptures();
  if (!captures.empty()) {
    
    llvm::SmallVector<Value, 4> capturedArgs;
    for (ValueDecl *capture : captures) {
      switch (getDeclCaptureKind(capture)) {
        case CaptureKind::LValue: {
          // LValues are captured as both the box owning the value and the
          // address of the value.
          assert(VarLocs.count(capture) &&
                 "no location for captured var!");
          
          VarLoc const &vl = VarLocs[capture];
          assert(vl.box && "no box for captured var!");
          assert(vl.address && "no address for captured var!");
          B.createRetain(loc, vl.box);
          capturedArgs.push_back(vl.box);
          capturedArgs.push_back(vl.address);
          break;
        }
        case CaptureKind::Byref: {
          // Byrefs are captured by address only.
          assert(VarLocs.count(capture) &&
                 "no location for captured byref!");
          capturedArgs.push_back(VarLocs[capture].address);
          break;
        }
        case CaptureKind::Constant: {
          // Value is a constant such as a local func. Pass on the reference.
          ManagedValue v = emitReferenceToDecl(loc, capture);
          capturedArgs.push_back(v.forward(*this));
          break;
        }
        case CaptureKind::GetterSetter: {
          // Pass the setter and getter closure references on.
          ManagedValue v = emitConstantRef(loc, SILConstant(capture,
                                                   SILConstant::Kind::Setter));
          capturedArgs.push_back(v.forward(*this));
          /* FALLTHROUGH */
        }
        case CaptureKind::Getter: {
          // Pass the getter closure reference on.
          ManagedValue v = emitConstantRef(loc, SILConstant(capture,
                                                   SILConstant::Kind::Getter));
          capturedArgs.push_back(v.forward(*this));
          break;
        }
      }
    }
    
    Value functionRef = emitGlobalConstantRef(loc, constant);
    Type closureSwiftTy
      = functionRef.getType().getAs<AnyFunctionType>()->getResult();
    SILType closureTy = getLoweredLoadableType(closureSwiftTy);
    return emitManagedRValueWithCleanup(
                    B.createPartialApply(loc, functionRef, capturedArgs,
                                         closureTy));
  } else {
    return ManagedValue(emitGlobalConstantRef(loc, constant));
  }
}

ManagedValue SILGenFunction::visitFuncExpr(FuncExpr *e, SGFContext C) {
  // Generate the local function body.
  SGM.emitFunction(e, e);

  // Generate the closure (if any) for the function reference.
  return emitClosureForCapturingExpr(e, SILConstant(e), e);
}

ManagedValue SILGenFunction::visitClosureExpr(ClosureExpr *e, SGFContext C) {
  // Generate the closure body.
  SGM.emitClosure(e);
  
  // Generate the closure value (if any) for the closure expr's function
  // reference.
  return emitClosureForCapturingExpr(e, SILConstant(e), e);
}

void SILGenFunction::emitFunction(FuncExpr *fe) {
  emitProlog(fe, fe->getBodyParamPatterns(), fe->getResultType(F.getContext()));
  visit(fe->getBody());
}

void SILGenFunction::emitClosure(ClosureExpr *ce) {
  emitProlog(ce, ce->getParamPatterns(),
             ce->getType()->castTo<FunctionType>()->getResult());

  // Closure expressions implicitly return the result of their body expression.
  // FIXME: address-only return from closure. refactor common code from
  // visitReturnStmt
  Value result;
  {
    FullExpr scope(Cleanups);
    result = visit(ce->getBody()).forward(*this);
  }
  if (B.hasValidInsertionPoint())
    Cleanups.emitReturnAndCleanups(ce->getBody(), result);
}

bool SILGenFunction::emitEpilogBB(SILLocation loc) {
  assert(epilogBB && "no epilog bb to emit?!");
  
  // If the epilog was not branched to at all, just unwind like a "return"
  // and emit the epilog into the current BB.
  if (epilogBB->pred_empty()) {
    epilogBB->eraseFromParent();

    // If the current bb is terminated then the epilog is just unreachable.
    if (!B.hasValidInsertionPoint())
      return false;
    
    Cleanups.emitCleanupsForReturn(loc);
  } else {
    // If the body didn't explicitly return, we need to branch out of it as if
    // returning. emitReturnAndCleanups will do that.
    if (B.hasValidInsertionPoint())
      Cleanups.emitReturnAndCleanups(loc, Value());
    // Emit the epilog into the epilog bb.
    B.emitBlock(epilogBB);
  }
  return true;
}

void SILGenFunction::emitDestructor(ClassDecl *cd, DestructorDecl *dd) {
  Value thisValue = emitDestructorProlog(cd, dd);

  // Create a basic block to jump to for the implicit destruction behavior
  // of releasing the elements and calling the base class destructor.
  // We won't actually emit the block until we finish with the destructor body.
  epilogBB = new (SGM.M) BasicBlock(&F, "destructor");
  
  // Emit the destructor body, if any.
  if (dd)
    visit(dd->getBody());
  
  if (!emitEpilogBB(dd))
    return;
  
  // Release our members.
  // FIXME: generic params
  // FIXME: Can a destructor always consider its fields fragile like this?
  for (Decl *member : cd->getMembers()) {
    if (VarDecl *vd = dyn_cast<VarDecl>(member)) {
      if (vd->isProperty())
        continue;
      TypeLoweringInfo const &ti = getTypeLoweringInfo(vd->getType());
      if (!ti.isTrivial()) {
        Value addr = B.createRefElementAddr(dd, thisValue, vd,
                                          ti.getLoweredType().getAddressType());
        if (ti.isAddressOnly()) {
          B.createDestroyAddr(dd, addr);
        } else {
          Value field = B.createLoad(dd, addr);
          emitReleaseRValue(dd, field);
        }
      }
    }
  }
  
  // If we have a base class, invoke its destructor.
  if (Type baseTy = cd->getBaseClass()) {
    SILConstant dtorConstant =
      SILConstant(baseTy->getClassOrBoundGenericClass(),
                  SILConstant::Kind::Destructor);
    Value baseThis = B.createUpcast(dd,
                                    thisValue,
                                    getLoweredLoadableType(baseTy));
    ManagedValue dtorValue = emitMethodRef(dd, baseThis, dtorConstant);
    B.createApply(dd, dtorValue.forward(*this),
                  SILType::getEmptyTupleType(SGM.M.getContext()),
                  baseThis);
  }
  
  B.createReturn(dd, B.createEmptyTuple(dd));
}

namespace {
  class CleanupMaterializeThisValue : public Cleanup {
    ConstructorDecl *ctor;
    Value thisLV;
    Value &thisValue;
  public:
    CleanupMaterializeThisValue(ConstructorDecl *ctor,
                                Value thisLV, Value &thisValue)
      : ctor(ctor), thisLV(thisLV), thisValue(thisValue) {}
    
    void emit(SILGenFunction &gen) override {
      thisValue = gen.B.createLoad(ctor, thisLV);
      gen.B.createDeallocVar(ctor, AllocKind::Stack, thisLV);
    }
  };
}


static void emitConstructorMetatypeArg(SILGenFunction &gen,
                                       ConstructorDecl *ctor) {
  // In addition to the declared arguments, the constructor implicitly takes
  // the metatype as its first argument, like a static function.
  Type metatype = ctor->getType()->castTo<AnyFunctionType>()->getInput();
  new (gen.F.getModule()) BBArgument(gen.getLoweredType(metatype),
                                     gen.F.begin());
}

void SILGenFunction::emitValueConstructor(ConstructorDecl *ctor) {
  // Emit the prolog.
  emitConstructorMetatypeArg(*this, ctor);
  emitProlog(ctor->getArguments(), ctor->getImplicitThisDecl()->getType());
  
  //
  // Create the 'this' value.
  //
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredType(thisDecl->getType());
  Value thisLV, thisValue;
  assert(!ctor->getAllocThisExpr() && "alloc_this expr for value type?!");
  assert(!thisTy.hasReferenceSemantics() && "can't emit a ref type ctor here");
  if (thisTy.isAddressOnly()) {
    // If 'this' is of an address-only value type, then we can construct the
    // indirect return argument directly.
    assert(IndirectReturnAddress &&
           "address-only constructor without indirect return?!");
    thisLV = IndirectReturnAddress;
  } else {
    // Materialize a temporary for the loadable value type.
    thisLV = B.createAllocVar(ctor, AllocKind::Stack,
                                 thisTy);
    // Set up a cleanup to load the final value at the end of the constructor
    // body.
    Cleanups.pushCleanup<CleanupMaterializeThisValue>(ctor, thisLV, thisValue);
  }
  VarLocs[thisDecl] = {Value(), thisLV};
  
  // Create a basic block to jump to for the implicit 'this' return.
  // We won't emit this until after we've emitted the body.
  epilogBB = new (SGM.M) BasicBlock(&F, "constructor");

  // Emit the constructor body.
  visit(ctor->getBody());
  
  // Return 'this' in the epilog.
  if (!emitEpilogBB(ctor))
    return;

  if (thisTy.isAddressOnly()) {
    // We already initialized the return value in-place.
    B.createReturn(ctor, B.createEmptyTuple(ctor));
  } else {
    assert(thisValue && "thisValue not initialized?!");
    B.createReturn(ctor, thisValue);
  }
}

namespace {
  // Unlike the ArgumentInitVisitor, this visitor generates arguments but leaves
  // them destructured instead of storing them to lvalues so that the
  // argument set can be easily forwarded to another function.
  class ArgumentForwardVisitor
    : public PatternVisitor<ArgumentForwardVisitor>
  {
    SILGenFunction &gen;
    SmallVectorImpl<Value> &args;
  public:
    ArgumentForwardVisitor(SILGenFunction &gen,
                           SmallVectorImpl<Value> &args)
      : gen(gen), args(args) {}
    
    void makeArgument(Type ty) {
      assert(ty && "no type?!");
      // Destructure tuple arguments.
      if (TupleType *tupleTy = ty->getAs<TupleType>()) {
        for (auto &field : tupleTy->getFields())
          makeArgument(field.getType());
      } else {
        Value arg = new (gen.F.getModule()) BBArgument(gen.getLoweredType(ty),
                                                       gen.F.begin());
        args.push_back(arg);
      }
    }

    void visitParenPattern(ParenPattern *P) {
      visit(P->getSubPattern());
    }
    
    void visitTypedPattern(TypedPattern *P) {
      visit(P->getSubPattern());
    }
    
    void visitTuplePattern(TuplePattern *P) {
      for (auto &elt : P->getFields())
        visit(elt.getPattern());
    }
    
    void visitAnyPattern(AnyPattern *P) {
      makeArgument(P->getType());
    }
    
    void visitNamedPattern(NamedPattern *P) {
      makeArgument(P->getType());
    }
  };
} // end anonymous namespace

void SILGenFunction::emitClassConstructorAllocator(ConstructorDecl *ctor) {
  // Emit the prolog. Since we're just going to forward our args directly
  // to the initializer, don't allocate local variables for them.
  emitConstructorMetatypeArg(*this, ctor);

  SmallVector<Value, 8> args;
  
  // Allocate the "this" value.
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredType(thisDecl->getType());
  assert(thisTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");
  Value thisValue;
  if (ctor->getAllocThisExpr()) {
    FullExpr allocThisScope(Cleanups);
    // If the constructor has an alloc-this expr, emit it to get "this".
    thisValue = visit(ctor->getAllocThisExpr()).forward(*this);
    assert(thisValue.getType() == thisTy &&
           "alloc-this expr type did not match this type?!");
  } else {
    // Otherwise, just emit an alloc_ref instruction for the default allocation
    // path.
    // FIXME: should have a cleanup in case of exception
    thisValue = B.createAllocRef(ctor, AllocKind::Heap, thisTy);
  }
  args.push_back(thisValue);

  // Forward the constructor arguments.
  ArgumentForwardVisitor(*this, args).visit(ctor->getArguments());

  // Call the initializer.
  SILConstant initConstant = SILConstant(ctor, SILConstant::Kind::Initializer);
  ManagedValue initVal = emitMethodRef(ctor, thisValue, initConstant);
  
  Value initedThisValue
    = B.createApply(ctor, initVal.forward(*this), thisTy, args);
  
  // Return the initialized 'this'.
  B.createReturn(ctor, initedThisValue);
}

void SILGenFunction::emitClassConstructorInitializer(ConstructorDecl *ctor) {
  // Emit the 'this' argument and make an lvalue for it.
  VarDecl *thisDecl = ctor->getImplicitThisDecl();
  SILType thisTy = getLoweredType(thisDecl->getType());
  Value thisValue = new (SGM.M) BBArgument(thisTy, F.begin());
  assert(thisTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");
  Value thisLV = emitTemporaryAllocation(ctor, thisTy);
  B.createRetain(ctor, thisValue);
  emitStore(ctor, ManagedValue(thisValue), thisLV);
  Cleanups.pushCleanup<CleanupMaterializedValue>(thisLV);
  VarLocs[thisDecl] = {Value(), thisLV};
  
  // Emit the prolog for the non-this arguments.
  emitProlog(ctor->getArguments(), TupleType::getEmpty(F.getContext()));

  // Create a basic block to jump to for the implicit 'this' return.
  // We won't emit the block until after we've emitted the body.
  epilogBB = new (SGM.M) BasicBlock(&F, "constructor");
  
  // Emit the constructor body.
  visit(ctor->getBody());
  
  // Return 'this' in the epilog.
  if (!emitEpilogBB(ctor))
    return;
  
  B.createReturn(ctor, thisValue);
}


ManagedValue SILGenFunction::visitInterpolatedStringLiteralExpr(
                                              InterpolatedStringLiteralExpr *E,
                                              SGFContext C)
{
  return visit(E->getSemanticExpr());
}

ManagedValue SILGenFunction::visitCollectionExpr(CollectionExpr *E,
                                                 SGFContext C) {
  return visit(E->getSemanticExpr());
}

ManagedValue SILGenFunction::visitRebindThisInConstructorExpr(
                                RebindThisInConstructorExpr *E, SGFContext C) {
  // FIXME: Use a different instruction from 'downcast'. IRGen can make
  // "rebind this" into a no-op if the called constructor is a Swift one.
  ManagedValue newThis = visit(E->getSubExpr());
  if (!newThis.getType().getSwiftRValueType()
        ->isEqual(E->getThis()->getType())) {
    assert(!newThis.getType().isAddress() &&
           newThis.getType().hasReferenceSemantics() &&
           "delegating ctor type mismatch for non-reference type?!");
    CleanupsDepth newThisCleanup = newThis.getCleanup();
    Value newThisValue = B.createDowncast(E, newThis.getValue(),
                              getLoweredLoadableType(E->getThis()->getType()));
    newThis = ManagedValue(newThisValue, newThisCleanup);
  }
  
  Value thisAddr = emitReferenceToDecl(E, E->getThis()).getUnmanagedValue();
  emitAssignPhysicalAddress(E, newThis, thisAddr);
  
  return ManagedValue(B.createEmptyTuple(E));
}

ManagedValue SILGenFunction::visitArchetypeSubscriptExpr(
                                     ArchetypeSubscriptExpr *E, SGFContext C) {
  llvm_unreachable("not implemented");
}

ManagedValue SILGenFunction::visitExistentialSubscriptExpr(
                                   ExistentialSubscriptExpr *E, SGFContext C) {
  llvm_unreachable("not implemented");
}

ManagedValue SILGenFunction::visitBridgeToBlockExpr(BridgeToBlockExpr *E,
                                                    SGFContext C) {
  llvm_unreachable("not implemented");
}

ManagedValue SILGenFunction::visitIfExpr(IfExpr *E, SGFContext C) {
  llvm_unreachable("not implemented");
}

Value SILGenFunction::emitThickenFunction(SILLocation loc, Value thinFn) {
  Type thickTy = getThickFunctionType(thinFn.getType().getSwiftType());
  
  return B.createThinToThickFunction(loc, thinFn,
                                     getLoweredLoadableType(thickTy));
}

void SILGenFunction::emitStore(SILLocation loc, ManagedValue src,
                               Value destAddr) {
  Value fwdSrc = src.forward(*this);
  // If we store a function value, we lose its thinness.
  // FIXME: This should go away when Swift typechecking learns how to handle
  // thin functions.
  if (src.getType().is<AnyFunctionType>() &&
      src.getType().castTo<AnyFunctionType>()->isThin()) {
    assert(!destAddr.getType().getObjectType().castTo<AnyFunctionType>()
             ->isThin()
           && "remove this when Swift actually emits thin function types");
    
    fwdSrc = emitThickenFunction(loc, fwdSrc);
  }
  
  B.createStore(loc, fwdSrc, destAddr);
}