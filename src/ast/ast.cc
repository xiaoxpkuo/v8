// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ast/ast.h"

#include <cmath>  // For isfinite.

#include "src/ast/compile-time-value.h"
#include "src/ast/prettyprinter.h"
#include "src/ast/scopes.h"
#include "src/base/hashmap.h"
#include "src/builtins/builtins-constructor.h"
#include "src/builtins/builtins.h"
#include "src/code-stubs.h"
#include "src/contexts.h"
#include "src/conversions.h"
#include "src/double.h"
#include "src/elements.h"
#include "src/objects-inl.h"
#include "src/objects/literal-objects.h"
#include "src/objects/map.h"
#include "src/property-details.h"
#include "src/property.h"
#include "src/string-stream.h"

namespace v8 {
namespace internal {

// ----------------------------------------------------------------------------
// Implementation of other node functionality.

#ifdef DEBUG

static const char* NameForNativeContextIntrinsicIndex(uint32_t idx) {
  switch (idx) {
#define NATIVE_CONTEXT_FIELDS_IDX(NAME, Type, name) \
  case Context::NAME:                               \
    return #name;

    NATIVE_CONTEXT_FIELDS(NATIVE_CONTEXT_FIELDS_IDX)
#undef NATIVE_CONTEXT_FIELDS_IDX

    default:
      break;
  }

  return "UnknownIntrinsicIndex";
}

void AstNode::Print() { Print(Isolate::Current()); }

void AstNode::Print(Isolate* isolate) {
  AllowHandleDereference allow_deref;
  AstPrinter::PrintOut(isolate, this);
}


#endif  // DEBUG

#define RETURN_NODE(Node) \
  case k##Node:           \
    return static_cast<Node*>(this);

IterationStatement* AstNode::AsIterationStatement() {
  switch (node_type()) {
    ITERATION_NODE_LIST(RETURN_NODE);
    default:
      return nullptr;
  }
}

BreakableStatement* AstNode::AsBreakableStatement() {
  switch (node_type()) {
    BREAKABLE_NODE_LIST(RETURN_NODE);
    ITERATION_NODE_LIST(RETURN_NODE);
    default:
      return nullptr;
  }
}

MaterializedLiteral* AstNode::AsMaterializedLiteral() {
  switch (node_type()) {
    LITERAL_NODE_LIST(RETURN_NODE);
    default:
      return nullptr;
  }
}

#undef RETURN_NODE

bool Expression::IsSmiLiteral() const {
  return IsLiteral() && AsLiteral()->IsSmi();
}

bool Expression::IsNumberLiteral() const {
  return IsLiteral() && AsLiteral()->IsNumber();
}

bool Expression::IsStringLiteral() const {
  return IsLiteral() && AsLiteral()->IsString();
}

bool Expression::IsPropertyName() const {
  return IsLiteral() && AsLiteral()->IsPropertyName();
}

bool Expression::IsNullLiteral() const {
  return IsLiteral() && AsLiteral()->IsNull();
}

bool Expression::IsTheHoleLiteral() const {
  return IsLiteral() && AsLiteral()->IsTheHole();
}

bool Expression::IsUndefinedLiteral() const {
  if (IsLiteral() && AsLiteral()->IsUndefined()) return true;

  const VariableProxy* var_proxy = AsVariableProxy();
  if (var_proxy == nullptr) return false;
  Variable* var = var_proxy->var();
  // The global identifier "undefined" is immutable. Everything
  // else could be reassigned.
  return var != nullptr && var->IsUnallocated() &&
         var_proxy->raw_name()->IsOneByteEqualTo("undefined");
}

bool Expression::ToBooleanIsTrue() const {
  return IsLiteral() && AsLiteral()->ToBooleanIsTrue();
}

bool Expression::ToBooleanIsFalse() const {
  return IsLiteral() && AsLiteral()->ToBooleanIsFalse();
}

bool Expression::IsValidReferenceExpression() const {
  // We don't want expressions wrapped inside RewritableExpression to be
  // considered as valid reference expressions, as they will be rewritten
  // to something (most probably involving a do expression).
  if (IsRewritableExpression()) return false;
  return IsProperty() ||
         (IsVariableProxy() && AsVariableProxy()->IsValidReferenceExpression());
}

bool Expression::IsAnonymousFunctionDefinition() const {
  return (IsFunctionLiteral() &&
          AsFunctionLiteral()->IsAnonymousFunctionDefinition()) ||
         (IsClassLiteral() &&
          AsClassLiteral()->IsAnonymousFunctionDefinition());
}

bool Expression::IsConciseMethodDefinition() const {
  return IsFunctionLiteral() && IsConciseMethod(AsFunctionLiteral()->kind());
}

bool Expression::IsAccessorFunctionDefinition() const {
  return IsFunctionLiteral() && IsAccessorFunction(AsFunctionLiteral()->kind());
}

bool Statement::IsJump() const {
  switch (node_type()) {
#define JUMP_NODE_LIST(V) \
  V(Block)                \
  V(ExpressionStatement)  \
  V(ContinueStatement)    \
  V(BreakStatement)       \
  V(ReturnStatement)      \
  V(IfStatement)
#define GENERATE_CASE(Node) \
  case k##Node:             \
    return static_cast<const Node*>(this)->IsJump();
    JUMP_NODE_LIST(GENERATE_CASE)
#undef GENERATE_CASE
#undef JUMP_NODE_LIST
    default:
      return false;
  }
}

VariableProxy::VariableProxy(Variable* var, int start_position)
    : Expression(start_position, kVariableProxy),
      raw_name_(var->raw_name()),
      next_unresolved_(nullptr) {
  bit_field_ |= IsThisField::encode(var->is_this()) |
                IsAssignedField::encode(false) |
                IsResolvedField::encode(false) |
                HoleCheckModeField::encode(HoleCheckMode::kElided);
  BindTo(var);
}

VariableProxy::VariableProxy(const VariableProxy* copy_from)
    : Expression(copy_from->position(), kVariableProxy),
      next_unresolved_(nullptr) {
  bit_field_ = copy_from->bit_field_;
  DCHECK(!copy_from->is_resolved());
  raw_name_ = copy_from->raw_name_;
}

void VariableProxy::BindTo(Variable* var) {
  DCHECK((is_this() && var->is_this()) || raw_name() == var->raw_name());
  set_var(var);
  set_is_resolved();
  var->set_is_used();
  if (is_assigned()) var->set_maybe_assigned();
}

Assignment::Assignment(NodeType node_type, Token::Value op, Expression* target,
                       Expression* value, int pos)
    : Expression(pos, node_type), target_(target), value_(value) {
  bit_field_ |= TokenField::encode(op);
}

bool FunctionLiteral::ShouldEagerCompile() const {
  return scope()->ShouldEagerCompile();
}

void FunctionLiteral::SetShouldEagerCompile() {
  scope()->set_should_eager_compile();
}

bool FunctionLiteral::AllowsLazyCompilation() {
  return scope()->AllowsLazyCompilation();
}

Handle<String> FunctionLiteral::name(Isolate* isolate) const {
  return raw_name_ ? raw_name_->string() : isolate->factory()->empty_string();
}

int FunctionLiteral::start_position() const {
  return scope()->start_position();
}


int FunctionLiteral::end_position() const {
  return scope()->end_position();
}


LanguageMode FunctionLiteral::language_mode() const {
  return scope()->language_mode();
}

FunctionKind FunctionLiteral::kind() const { return scope()->function_kind(); }

bool FunctionLiteral::NeedsHomeObject(Expression* expr) {
  if (expr == nullptr || !expr->IsFunctionLiteral()) return false;
  DCHECK_NOT_NULL(expr->AsFunctionLiteral()->scope());
  return expr->AsFunctionLiteral()->scope()->NeedsHomeObject();
}

ObjectLiteralProperty::ObjectLiteralProperty(Expression* key, Expression* value,
                                             Kind kind, bool is_computed_name)
    : LiteralProperty(key, value, is_computed_name),
      kind_(kind),
      emit_store_(true) {}

ObjectLiteralProperty::ObjectLiteralProperty(AstValueFactory* ast_value_factory,
                                             Expression* key, Expression* value,
                                             bool is_computed_name)
    : LiteralProperty(key, value, is_computed_name), emit_store_(true) {
  if (!is_computed_name && key->AsLiteral()->IsString() &&
      key->AsLiteral()->AsRawString() == ast_value_factory->proto_string()) {
    kind_ = PROTOTYPE;
  } else if (value_->AsMaterializedLiteral() != nullptr) {
    kind_ = MATERIALIZED_LITERAL;
  } else if (value_->IsLiteral()) {
    kind_ = CONSTANT;
  } else {
    kind_ = COMPUTED;
  }
}

bool LiteralProperty::NeedsSetFunctionName() const {
  return is_computed_name_ && (value_->IsAnonymousFunctionDefinition() ||
                               value_->IsConciseMethodDefinition() ||
                               value_->IsAccessorFunctionDefinition());
}

ClassLiteralProperty::ClassLiteralProperty(Expression* key, Expression* value,
                                           Kind kind, bool is_static,
                                           bool is_computed_name)
    : LiteralProperty(key, value, is_computed_name),
      kind_(kind),
      is_static_(is_static) {}

bool ObjectLiteral::Property::IsCompileTimeValue() const {
  return kind_ == CONSTANT ||
      (kind_ == MATERIALIZED_LITERAL &&
       CompileTimeValue::IsCompileTimeValue(value_));
}


void ObjectLiteral::Property::set_emit_store(bool emit_store) {
  emit_store_ = emit_store;
}

bool ObjectLiteral::Property::emit_store() const { return emit_store_; }

void ObjectLiteral::CalculateEmitStore(Zone* zone) {
  const auto GETTER = ObjectLiteral::Property::GETTER;
  const auto SETTER = ObjectLiteral::Property::SETTER;

  ZoneAllocationPolicy allocator(zone);

  CustomMatcherZoneHashMap table(
      Literal::Match, ZoneHashMap::kDefaultHashMapCapacity, allocator);
  for (int i = properties()->length() - 1; i >= 0; i--) {
    ObjectLiteral::Property* property = properties()->at(i);
    if (property->is_computed_name()) continue;
    if (property->IsPrototype()) continue;
    Literal* literal = property->key()->AsLiteral();
    DCHECK(!literal->IsNullLiteral());

    // If there is an existing entry do not emit a store unless the previous
    // entry was also an accessor.
    uint32_t hash = literal->Hash();
    ZoneHashMap::Entry* entry = table.LookupOrInsert(literal, hash, allocator);
    if (entry->value != nullptr) {
      auto previous_kind =
          static_cast<ObjectLiteral::Property*>(entry->value)->kind();
      if (!((property->kind() == GETTER && previous_kind == SETTER) ||
            (property->kind() == SETTER && previous_kind == GETTER))) {
        property->set_emit_store(false);
      }
    }
    entry->value = property;
  }
}

void ObjectLiteral::InitFlagsForPendingNullPrototype(int i) {
  // We still check for __proto__:null after computed property names.
  for (; i < properties()->length(); i++) {
    if (properties()->at(i)->IsNullPrototype()) {
      set_has_null_protoype(true);
      break;
    }
  }
}

int ObjectLiteral::InitDepthAndFlags() {
  if (is_initialized()) return depth();
  bool is_simple = true;
  bool has_seen_prototype = false;
  bool needs_initial_allocation_site = false;
  int depth_acc = 1;
  uint32_t nof_properties = 0;
  uint32_t elements = 0;
  uint32_t max_element_index = 0;
  for (int i = 0; i < properties()->length(); i++) {
    ObjectLiteral::Property* property = properties()->at(i);
    if (property->IsPrototype()) {
      has_seen_prototype = true;
      // __proto__:null has no side-effects and is set directly on the
      // boilerplate.
      if (property->IsNullPrototype()) {
        set_has_null_protoype(true);
        continue;
      }
      DCHECK(!has_null_prototype());
      is_simple = false;
      continue;
    }
    if (nof_properties == boilerplate_properties_) {
      DCHECK(property->is_computed_name());
      is_simple = false;
      if (!has_seen_prototype) InitFlagsForPendingNullPrototype(i);
      break;
    }
    DCHECK(!property->is_computed_name());

    MaterializedLiteral* literal = property->value()->AsMaterializedLiteral();
    if (literal != nullptr) {
      int subliteral_depth = literal->InitDepthAndFlags() + 1;
      if (subliteral_depth > depth_acc) depth_acc = subliteral_depth;
      needs_initial_allocation_site |= literal->NeedsInitialAllocationSite();
    }

    Literal* key = property->key()->AsLiteral();
    Expression* value = property->value();

    bool is_compile_time_value = CompileTimeValue::IsCompileTimeValue(value);
    is_simple = is_simple && is_compile_time_value;

    // Keep track of the number of elements in the object literal and
    // the largest element index.  If the largest element index is
    // much larger than the number of elements, creating an object
    // literal with fast elements will be a waste of space.
    uint32_t element_index = 0;
    if (key->IsString() && key->AsRawString()->AsArrayIndex(&element_index)) {
      max_element_index = Max(element_index, max_element_index);
      elements++;
    } else if (key->ToArrayIndex(&element_index)) {
      max_element_index = Max(element_index, max_element_index);
      elements++;
    }

    nof_properties++;
  }

  set_depth(depth_acc);
  set_is_simple(is_simple);
  set_needs_initial_allocation_site(needs_initial_allocation_site);
  set_has_elements(elements > 0);
  set_fast_elements((max_element_index <= 32) ||
                    ((2 * elements) >= max_element_index));
  return depth_acc;
}

void ObjectLiteral::BuildConstantProperties(Isolate* isolate) {
  if (!constant_properties_.is_null()) return;

  int index_keys = 0;
  bool has_seen_proto = false;
  for (int i = 0; i < properties()->length(); i++) {
    ObjectLiteral::Property* property = properties()->at(i);
    if (property->IsPrototype()) {
      has_seen_proto = true;
      continue;
    }
    if (property->is_computed_name()) {
      continue;
    }

    Handle<Object> key = property->key()->AsLiteral()->value();

    uint32_t element_index = 0;
    if (key->ToArrayIndex(&element_index) ||
        (key->IsString() && String::cast(*key)->AsArrayIndex(&element_index))) {
      index_keys++;
    }
  }

  Handle<BoilerplateDescription> constant_properties =
      isolate->factory()->NewBoilerplateDescription(boilerplate_properties_,
                                                    properties()->length(),
                                                    index_keys, has_seen_proto);

  int position = 0;
  for (int i = 0; i < properties()->length(); i++) {
    ObjectLiteral::Property* property = properties()->at(i);
    if (property->IsPrototype()) continue;

    if (static_cast<uint32_t>(position) == boilerplate_properties_ * 2) {
      DCHECK(property->is_computed_name());
      break;
    }
    DCHECK(!property->is_computed_name());

    MaterializedLiteral* m_literal = property->value()->AsMaterializedLiteral();
    if (m_literal != nullptr) {
      m_literal->BuildConstants(isolate);
    }

    // Add CONSTANT and COMPUTED properties to boilerplate. Use undefined
    // value for COMPUTED properties, the real value is filled in at
    // runtime. The enumeration order is maintained.
    Handle<Object> key = property->key()->AsLiteral()->value();
    Handle<Object> value = GetBoilerplateValue(property->value(), isolate);

    uint32_t element_index = 0;
    if (key->IsString() && String::cast(*key)->AsArrayIndex(&element_index)) {
      key = isolate->factory()->NewNumberFromUint(element_index);
    } else if (key->IsNumber() && !key->ToArrayIndex(&element_index)) {
      key = isolate->factory()->NumberToString(key);
    }

    // Add name, value pair to the fixed array.
    constant_properties->set(position++, *key);
    constant_properties->set(position++, *value);
  }

  constant_properties_ = constant_properties;
}

bool ObjectLiteral::IsFastCloningSupported() const {
  // The CreateShallowObjectLiteratal builtin doesn't copy elements, and object
  // literals don't support copy-on-write (COW) elements for now.
  // TODO(mvstanton): make object literals support COW elements.
  return fast_elements() && is_shallow() &&
         properties_count() <=
             ConstructorBuiltins::kMaximumClonedShallowObjectProperties;
}

bool ArrayLiteral::is_empty() const {
  DCHECK(is_initialized());
  return values()->is_empty() &&
         (constant_elements().is_null() || constant_elements()->is_empty());
}

int ArrayLiteral::InitDepthAndFlags() {
  DCHECK_LT(first_spread_index_, 0);
  if (is_initialized()) return depth();

  int constants_length = values()->length();

  // Fill in the literals.
  bool is_simple = true;
  int depth_acc = 1;
  int array_index = 0;
  for (; array_index < constants_length; array_index++) {
    Expression* element = values()->at(array_index);
    DCHECK(!element->IsSpread());
    MaterializedLiteral* literal = element->AsMaterializedLiteral();
    if (literal != nullptr) {
      int subliteral_depth = literal->InitDepthAndFlags() + 1;
      if (subliteral_depth > depth_acc) depth_acc = subliteral_depth;
    }

    if (!CompileTimeValue::IsCompileTimeValue(element)) {
      is_simple = false;
    }
  }

  set_depth(depth_acc);
  set_is_simple(is_simple);
  // Array literals always need an initial allocation site to properly track
  // elements transitions.
  set_needs_initial_allocation_site(true);
  return depth_acc;
}

void ArrayLiteral::BuildConstantElements(Isolate* isolate) {
  DCHECK_LT(first_spread_index_, 0);

  if (!constant_elements_.is_null()) return;

  int constants_length = values()->length();
  ElementsKind kind = FIRST_FAST_ELEMENTS_KIND;
  Handle<FixedArray> fixed_array =
      isolate->factory()->NewFixedArrayWithHoles(constants_length);

  // Fill in the literals.
  bool is_holey = false;
  int array_index = 0;
  for (; array_index < constants_length; array_index++) {
    Expression* element = values()->at(array_index);
    DCHECK(!element->IsSpread());
    MaterializedLiteral* m_literal = element->AsMaterializedLiteral();
    if (m_literal != nullptr) {
      m_literal->BuildConstants(isolate);
    }

    // New handle scope here, needs to be after BuildContants().
    HandleScope scope(isolate);
    Handle<Object> boilerplate_value = GetBoilerplateValue(element, isolate);
    if (boilerplate_value->IsTheHole(isolate)) {
      is_holey = true;
      continue;
    }

    if (boilerplate_value->IsUninitialized(isolate)) {
      boilerplate_value = handle(Smi::kZero, isolate);
    }

    kind = GetMoreGeneralElementsKind(kind,
                                      boilerplate_value->OptimalElementsKind());
    fixed_array->set(array_index, *boilerplate_value);
  }

  if (is_holey) kind = GetHoleyElementsKind(kind);

  // Simple and shallow arrays can be lazily copied, we transform the
  // elements array to a copy-on-write array.
  if (is_simple() && depth() == 1 && array_index > 0 &&
      IsSmiOrObjectElementsKind(kind)) {
    fixed_array->set_map(isolate->heap()->fixed_cow_array_map());
  }

  Handle<FixedArrayBase> elements = fixed_array;
  if (IsDoubleElementsKind(kind)) {
    ElementsAccessor* accessor = ElementsAccessor::ForKind(kind);
    elements = isolate->factory()->NewFixedDoubleArray(constants_length);
    // We are copying from non-fast-double to fast-double.
    ElementsKind from_kind = TERMINAL_FAST_ELEMENTS_KIND;
    accessor->CopyElements(fixed_array, from_kind, elements, constants_length);
  }

  // Remember both the literal's constant values as well as the ElementsKind.
  Handle<ConstantElementsPair> literals =
      isolate->factory()->NewConstantElementsPair(kind, elements);

  constant_elements_ = literals;
}

bool ArrayLiteral::IsFastCloningSupported() const {
  return depth() <= 1 &&
         values()->length() <=
             ConstructorBuiltins::kMaximumClonedShallowArrayElements;
}

void ArrayLiteral::RewindSpreads() {
  values_->Rewind(first_spread_index_);
  first_spread_index_ = -1;
}

bool MaterializedLiteral::IsSimple() const {
  if (IsArrayLiteral()) return AsArrayLiteral()->is_simple();
  if (IsObjectLiteral()) return AsObjectLiteral()->is_simple();
  DCHECK(IsRegExpLiteral());
  return false;
}

Handle<Object> MaterializedLiteral::GetBoilerplateValue(Expression* expression,
                                                        Isolate* isolate) {
  if (expression->IsLiteral()) {
    return expression->AsLiteral()->value();
  }
  if (CompileTimeValue::IsCompileTimeValue(expression)) {
    return CompileTimeValue::GetValue(isolate, expression);
  }
  return isolate->factory()->uninitialized_value();
}

int MaterializedLiteral::InitDepthAndFlags() {
  if (IsArrayLiteral()) return AsArrayLiteral()->InitDepthAndFlags();
  if (IsObjectLiteral()) return AsObjectLiteral()->InitDepthAndFlags();
  DCHECK(IsRegExpLiteral());
  return 1;
}

bool MaterializedLiteral::NeedsInitialAllocationSite() {
  if (IsArrayLiteral()) {
    return AsArrayLiteral()->needs_initial_allocation_site();
  }
  if (IsObjectLiteral()) {
    return AsObjectLiteral()->needs_initial_allocation_site();
  }
  DCHECK(IsRegExpLiteral());
  return false;
}

void MaterializedLiteral::BuildConstants(Isolate* isolate) {
  if (IsArrayLiteral()) {
    return AsArrayLiteral()->BuildConstantElements(isolate);
  }
  if (IsObjectLiteral()) {
    return AsObjectLiteral()->BuildConstantProperties(isolate);
  }
  DCHECK(IsRegExpLiteral());
}

Handle<TemplateObjectDescription> GetTemplateObject::GetOrBuildDescription(
    Isolate* isolate) {
  Handle<FixedArray> raw_strings =
      isolate->factory()->NewFixedArray(this->raw_strings()->length(), TENURED);
  bool raw_and_cooked_match = true;
  for (int i = 0; i < raw_strings->length(); ++i) {
    if (this->cooked_strings()->at(i) == nullptr ||
        *this->raw_strings()->at(i)->string() !=
            *this->cooked_strings()->at(i)->string()) {
      raw_and_cooked_match = false;
    }
    raw_strings->set(i, *this->raw_strings()->at(i)->string());
  }
  Handle<FixedArray> cooked_strings = raw_strings;
  if (!raw_and_cooked_match) {
    cooked_strings = isolate->factory()->NewFixedArray(
        this->cooked_strings()->length(), TENURED);
    for (int i = 0; i < cooked_strings->length(); ++i) {
      if (this->cooked_strings()->at(i) != nullptr) {
        cooked_strings->set(i, *this->cooked_strings()->at(i)->string());
      } else {
        cooked_strings->set(i, isolate->heap()->undefined_value());
      }
    }
  }
  return isolate->factory()->NewTemplateObjectDescription(
      this->hash(), raw_strings, cooked_strings);
}

static bool IsCommutativeOperationWithSmiLiteral(Token::Value op) {
  // Add is not commutative due to potential for string addition.
  return op == Token::MUL || op == Token::BIT_AND || op == Token::BIT_OR ||
         op == Token::BIT_XOR;
}

// Check for the pattern: x + 1.
static bool MatchSmiLiteralOperation(Expression* left, Expression* right,
                                     Expression** expr, Smi** literal) {
  if (right->IsSmiLiteral()) {
    *expr = left;
    *literal = right->AsLiteral()->AsSmiLiteral();
    return true;
  }
  return false;
}

bool BinaryOperation::IsSmiLiteralOperation(Expression** subexpr,
                                            Smi** literal) {
  return MatchSmiLiteralOperation(left_, right_, subexpr, literal) ||
         (IsCommutativeOperationWithSmiLiteral(op()) &&
          MatchSmiLiteralOperation(right_, left_, subexpr, literal));
}

static bool IsTypeof(Expression* expr) {
  UnaryOperation* maybe_unary = expr->AsUnaryOperation();
  return maybe_unary != nullptr && maybe_unary->op() == Token::TYPEOF;
}

// Check for the pattern: typeof <expression> equals <string literal>.
static bool MatchLiteralCompareTypeof(Expression* left, Token::Value op,
                                      Expression* right, Expression** expr,
                                      Literal** literal) {
  if (IsTypeof(left) && right->IsStringLiteral() && Token::IsEqualityOp(op)) {
    *expr = left->AsUnaryOperation()->expression();
    *literal = right->AsLiteral();
    return true;
  }
  return false;
}

bool CompareOperation::IsLiteralCompareTypeof(Expression** expr,
                                              Literal** literal) {
  return MatchLiteralCompareTypeof(left_, op(), right_, expr, literal) ||
         MatchLiteralCompareTypeof(right_, op(), left_, expr, literal);
}


static bool IsVoidOfLiteral(Expression* expr) {
  UnaryOperation* maybe_unary = expr->AsUnaryOperation();
  return maybe_unary != nullptr && maybe_unary->op() == Token::VOID &&
         maybe_unary->expression()->IsLiteral();
}


// Check for the pattern: void <literal> equals <expression> or
// undefined equals <expression>
static bool MatchLiteralCompareUndefined(Expression* left,
                                         Token::Value op,
                                         Expression* right,
                                         Expression** expr) {
  if (IsVoidOfLiteral(left) && Token::IsEqualityOp(op)) {
    *expr = right;
    return true;
  }
  if (left->IsUndefinedLiteral() && Token::IsEqualityOp(op)) {
    *expr = right;
    return true;
  }
  return false;
}

bool CompareOperation::IsLiteralCompareUndefined(Expression** expr) {
  return MatchLiteralCompareUndefined(left_, op(), right_, expr) ||
         MatchLiteralCompareUndefined(right_, op(), left_, expr);
}

// Check for the pattern: null equals <expression>
static bool MatchLiteralCompareNull(Expression* left,
                                    Token::Value op,
                                    Expression* right,
                                    Expression** expr) {
  if (left->IsNullLiteral() && Token::IsEqualityOp(op)) {
    *expr = right;
    return true;
  }
  return false;
}

bool CompareOperation::IsLiteralCompareNull(Expression** expr) {
  return MatchLiteralCompareNull(left_, op(), right_, expr) ||
         MatchLiteralCompareNull(right_, op(), left_, expr);
}

Call::CallType Call::GetCallType() const {
  VariableProxy* proxy = expression()->AsVariableProxy();
  if (proxy != nullptr) {
    if (proxy->var()->IsUnallocated()) {
      return GLOBAL_CALL;
    } else if (proxy->var()->IsLookupSlot()) {
      // Calls going through 'with' always use DYNAMIC rather than DYNAMIC_LOCAL
      // or DYNAMIC_GLOBAL.
      return proxy->var()->mode() == DYNAMIC ? WITH_CALL : OTHER_CALL;
    }
  }

  if (expression()->IsSuperCallReference()) return SUPER_CALL;

  Property* property = expression()->AsProperty();
  if (property != nullptr) {
    bool is_super = property->IsSuperAccess();
    if (property->key()->IsPropertyName()) {
      return is_super ? NAMED_SUPER_PROPERTY_CALL : NAMED_PROPERTY_CALL;
    } else {
      return is_super ? KEYED_SUPER_PROPERTY_CALL : KEYED_PROPERTY_CALL;
    }
  }

  return OTHER_CALL;
}

CaseClause::CaseClause(Expression* label, ZoneList<Statement*>* statements)
    : label_(label), statements_(statements) {}

bool Literal::ToUint32(uint32_t* value) const {
  if (IsSmi()) {
    int num = AsSmiLiteral()->value();
    if (num < 0) return false;
    *value = static_cast<uint32_t>(num);
    return true;
  }
  if (IsNumber()) {
    return DoubleToUint32IfEqualToSelf(AsNumber(), value);
  }
  return false;
}

bool Literal::ToArrayIndex(uint32_t* value) const {
  return ToUint32(value) && *value != kMaxUInt32;
}

uint32_t Literal::Hash() {
  return IsString() ? AsRawString()->Hash()
                    : ComputeLongHash(double_to_uint64(AsNumber()));
}


// static
bool Literal::Match(void* literal1, void* literal2) {
  const AstValue* x = static_cast<Literal*>(literal1)->value_;
  const AstValue* y = static_cast<Literal*>(literal2)->value_;
  return (x->IsString() && y->IsString() && x->AsString() == y->AsString()) ||
         (x->IsNumber() && y->IsNumber() && x->AsNumber() == y->AsNumber());
}

const char* CallRuntime::debug_name() {
#ifdef DEBUG
  return is_jsruntime() ? NameForNativeContextIntrinsicIndex(context_index_)
                        : function_->name;
#else
  return is_jsruntime() ? "(context function)" : function_->name;
#endif  // DEBUG
}

#define RETURN_LABELS(NodeType) \
  case k##NodeType:             \
    return static_cast<const NodeType*>(this)->labels();

ZoneList<const AstRawString*>* BreakableStatement::labels() const {
  switch (node_type()) {
    BREAKABLE_NODE_LIST(RETURN_LABELS)
    ITERATION_NODE_LIST(RETURN_LABELS)
    default:
      UNREACHABLE();
  }
}

#undef RETURN_LABELS

}  // namespace internal
}  // namespace v8
