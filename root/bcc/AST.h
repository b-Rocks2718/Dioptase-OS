#ifndef AST_H
#define AST_H

#include "../crt/stddef.h"
#include "../crt/stdbool.h"

#include "slice.h"
#include "identifier_map.h"
#include "types.h"

struct AbstractArray;
struct AbstractDeclarator;
struct AbstractFunction;
struct AbstractPointer;
struct ArgList;
struct Block;
struct BlockItem;
struct CaseList;
struct Declaration;
struct DeclarationList;
struct Declarator;
struct EnumMemberDclr;
struct Expr;
struct ForInit;
struct IdentMap;
struct Initializer;
struct InitializerList;
struct MemberDclr;
struct ParamInfoList;
struct ParamList;
struct Slice;
struct Statement;
struct StorageClassList;
struct Type;
struct TypeSpecList;

/* AST data structures */

struct Program {
  struct DeclarationList* dclrs;
};

// Link parameter type descriptors in declaration order.
struct ParamTypeList {
  struct Type* type;
  struct ParamTypeList* next;
};

// Classify the declaration payload stored in a Declaration.
enum DclrType {
  VAR_DCLR,
  FUN_DCLR,
  STRUCT_DCLR,
  UNION_DCLR,
  ENUM_DCLR,
  MEMBER_DCLR,
};

// Distinguish scalar and aggregate initializer forms.
enum InitializerType {
  SINGLE_INIT,
  COMPOUND_INIT,
};

// Link initializer elements in source order.
struct InitializerList {
  struct Initializer* init;
  struct InitializerList* next;
};

// Select either one expression or a nested initializer list.
union InitializerVariant {
  struct Expr* single_init;
  struct InitializerList* compound_init;
};

// Describe an initializer, its target type, and source location.
struct Initializer {
  enum InitializerType init_type;
  union InitializerVariant init;
  struct Type* type; // target type for this initializer
  char* loc; // location in source for error reporting
};

// Store the optional cleanup function attached to a variable.
struct VarAttributes {
  struct Slice* cleanup_func;
};

// Describe a variable's name, type, storage class, initializer, and attributes.
struct VariableDclr {
  struct Slice* name;
  struct Initializer* init;
  struct Type* type;
  enum StorageClass storage;
  struct VarAttributes attributes;
};

// Describe a function declaration, parameters, return type, and body.
struct FunctionDclr {
  struct Slice* name;
  enum StorageClass storage;
  struct ParamList* params;
  struct Type* type;
  struct Block* body;
};

// Describe one aggregate member and link it to the next member.
struct MemberDclr {
  struct Slice* name;
  struct Type* type;
  struct MemberDclr* next;
};

// Describe one enumerator's name/value pair and link the list.
struct EnumMemberDclr {
  struct Slice* name;
  int value;
  struct EnumMemberDclr* next;
};

// Describe a struct tag and its member declarations.
struct StructDclr {
  struct Slice* name;
  struct MemberDclr* members;
};

// Describe a union tag and its member declarations.
struct UnionDclr {
  struct Slice* name;
  struct MemberDclr* members;
};

// Describe an enum tag and its enumerator declarations.
struct EnumDclr {
  struct Slice* name;
  struct EnumMemberDclr* members;
};

// Select the concrete declaration payload identified by DclrType.
union DeclareVariant {
  struct VariableDclr var_dclr;
  struct FunctionDclr fun_dclr;
  struct StructDclr struct_dclr;
  struct UnionDclr union_dclr;
  struct EnumDclr enum_dclr;
  struct MemberDclr member_dclr;
};

// Link function parameter declarations in source order.
struct ParamList {
  struct VariableDclr param;
  struct ParamList* next;
};

// Pair a declaration payload with its concrete declaration kind.
struct Declaration {
  union DeclareVariant dclr;
  enum DclrType type;
};

// Link top-level or block declarations in source order.
struct DeclarationList {
  struct Declaration dclr;
  struct DeclarationList* next;
};

// Classify the concrete expression payload in an Expr.
enum ExprType {
  BINARY,
  ASSIGN,
  POST_ASSIGN,
  CONDITIONAL,
  LIT,
  UNARY,
  VAR,
  FUNCTION_CALL,
  CAST,
  ADDR_OF,
  DEREFERENCE,
  SUBSCRIPT,
  STRING,
  SIZEOF_EXPR,
  SIZEOF_T_EXPR,
  STMT_EXPR,
  DOT_EXPR,
  ARROW_EXPR,
};

// Enumerate arithmetic, logical, comparison, assignment, and comma operators.
enum BinOp {
  ADD_OP = 1,
  SUB_OP,
  MUL_OP,
  DIV_OP,
  MOD_OP,
  BIT_AND,
  BIT_OR,
  BIT_XOR,
  BIT_SHR,
  BIT_SHL,
  BOOL_AND,
  BOOL_OR,
  BOOL_EQ,
  BOOL_NEQ,
  BOOL_LE,
  BOOL_GE,
  BOOL_LEQ,
  BOOL_GEQ,
  ASSIGN_OP,
  PLUS_EQ_OP,
  MINUS_EQ_OP,
  MUL_EQ_OP,
  DIV_EQ_OP,
  MOD_EQ_OP,
  AND_EQ_OP,
  OR_EQ_OP,
  XOR_EQ_OP,
  SHL_EQ_OP,
  SHR_EQ_OP,
  TERNARY_OP,
  COMMA_OP,
};

// Enumerate prefix unary operators supported by the parser.
enum UnOp {
  COMPLEMENT = 1,
  NEGATE,
  BOOL_NOT,
  UNARY_PLUS,
};

// Describe a binary operator and its two operand expressions.
struct BinaryExpr {
  enum BinOp op;
  struct Expr* left;
  struct Expr* right;
};

// Describe the target and value expressions of an assignment.
struct AssignExpr {
  struct Expr* left;
  struct Expr* right;
};

// Enumerate postfix increment and decrement operators.
enum PostOp {
  POST_INC,
  POST_DEC
};

// Describe a postfix increment or decrement expression.
struct PostAssignExpr {
  enum PostOp op;
  struct Expr* expr;
};

// Describe the condition, true branch, and false branch of ?:.
struct ConditionalExpr {
  struct Expr* condition;
  struct Expr* left;
  struct Expr* right;
};

// Classify the integer width and signedness of a literal constant.
enum ConstType {
  INT_CONST,
  UINT_CONST,
  LONG_CONST,
  ULONG_CONST
};

// Store the integer representation selected by ConstType.
union ConstVariant {
  char char_val;
  short short_val;
  unsigned short ushort_val;
  int int_val;
  unsigned uint_val;
  long long_val;
  unsigned long ulong_val;
};

// Store a literal's integer kind and value.
struct LitExpr {
  enum ConstType type;
  union ConstVariant value;
};

// Describe a prefix unary operator and its operand.
struct UnaryExpr {
  enum UnOp op;
  struct Expr* expr;
};

// Store the referenced identifier for a variable expression.
struct VarExpr {
  struct Slice* name;
};

// Store a call target and its argument list.
struct FunctionCallExpr {
  struct Expr* func; // name or pointer
  struct ArgList* args;
};

// Store the destination type and operand of a cast.
struct CastExpr {
  struct Type* target;
  struct Expr* expr;
};

// Store the operand whose address is requested.
struct AddrOfExpr {
  struct Expr* expr;
};

// Store the pointer expression being dereferenced.
struct DereferenceExpr {
  struct Expr* expr;
};

// Store the indexed expression and its subscript.
struct SubscriptExpr {
  struct Expr* array;
  struct Expr* index;
};

// Store the source slice for a string literal expression.
struct StringExpr {
  struct Slice* string;
};

// Store the operand of sizeof(expression).
struct SizeOfExpr {
  struct Expr* expr;
};

// Store the type operand of sizeof(type).
struct SizeOfTExpr {
  struct Type* type;
};

// Store the block evaluated by a statement expression.
struct StmtExpr {
  struct Block* block;
};

// Store a base aggregate expression and selected member name.
struct DotExpr {
  struct Expr* struct_expr;
  struct Slice* member;
};

// Store a base pointer expression and selected member name.
struct ArrowExpr {
  struct Expr* pointer_expr;
  struct Slice* member;
};

// Select the concrete expression payload identified by ExprType.
union ExprVariant {
  struct BinaryExpr bin_expr;
  struct AssignExpr assign_expr;
  struct PostAssignExpr post_assign_expr;
  struct ConditionalExpr conditional_expr;
  struct LitExpr lit_expr;
  struct UnaryExpr un_expr;
  struct VarExpr var_expr;
  struct FunctionCallExpr fun_call_expr;
  struct CastExpr cast_expr;
  struct AddrOfExpr addr_of_expr;
  struct DereferenceExpr deref_expr;
  struct SubscriptExpr subscript_expr;
  struct StringExpr string_expr;
  struct SizeOfExpr sizeof_expr;
  struct SizeOfTExpr sizeof_t_expr;
  struct StmtExpr stmt_expr;
  struct DotExpr dot_expr;
  struct ArrowExpr arrow_expr;
};

// Store expression source location, inferred type, kind, and payload.
struct Expr {
  char* loc; // start of this expression in the preprocessed source
  struct Type* value_type;
  enum ExprType type;
  union ExprVariant expr;
};

// Link function-call arguments in evaluation order.
struct ArgList {
  struct Expr* arg;
  struct ArgList* next;
};

// Classify the concrete statement payload in a Statement.
enum StatementType {
  RETURN_STMT,
  EXPR_STMT,
  IF_STMT,
  GOTO_STMT,
  LABELED_STMT,
  COMPOUND_STMT,
  BREAK_STMT,
  CONTINUE_STMT,
  WHILE_STMT,
  DO_WHILE_STMT,
  FOR_STMT,
  SWITCH_STMT,
  CASE_STMT,
  DEFAULT_STMT,
  NULL_STMT
};

// Store a return expression and its owning function name.
struct ReturnStmt {
  struct Expr* expr;
  struct Slice* func;
};

// Store the expression evaluated for its side effects.
struct ExprStmt {
  struct Expr* expr;
};

// Store an if condition and its then/else statement branches.
struct IfStmt {
  struct Expr* condition;
  struct Statement* if_stmt;
  struct Statement* else_stmt;
};

// Store the target label of a goto statement.
struct GotoStmt {
  struct Slice* label;
};

// Associate a user label with the statement it prefixes.
struct LabeledStmt {
  struct Slice* label;
  struct Statement* stmt;
};

// Store the block items contained by a compound statement.
struct CompoundStmt {
  struct Block* block;
};

// Store the loop or switch label targeted by break.
struct BreakStmt {
  struct Slice* label;
};

// Store the loop label targeted by continue.
struct ContinueStmt {
  struct Slice* label;
};

// Store a while condition, body, and generated control-flow label.
struct WhileStmt {
  struct Expr* condition;
  struct Statement* statement;
  struct Slice* label;
};

// Store a do-while body, condition, and generated control-flow label.
struct DoWhileStmt {
  struct Statement* statement;
  struct Expr* condition;
  struct Slice* label;
};

// Store for-loop initializer, condition, increment, body, and labels.
struct ForStmt {
  struct ForInit* init;
  struct Expr* condition;
  struct Expr* end;
  struct Statement* statement;
  struct Slice* label;
  struct IdentMap* init_idents;
};

// Distinguish declaration and expression forms of a for initializer.
enum ForInitType {
  DCLR_INIT,
  EXPR_INIT,
};  

// Select declaration or expression form of a for-loop initializer.
union ForInitVariant {
  struct VariableDclr* dclr_init;
  struct Expr* expr_init;
};

// Store initializer kind and payload for a for-loop.
struct ForInit {
  enum ForInitType type;
  union ForInitVariant init;
};

// Store switch expression, body, labels, and case list.
struct SwitchStmt {
  struct Expr* condition;
  struct Statement* statement;
  struct Slice* label;
  struct CaseList* cases;
};

// Store a case constant, its statement, and generated label.
struct CaseStmt {
  struct Expr* expr;
  struct Statement* statement;
  struct Slice* label;
};

// Store the default statement and generated label.
struct DefaultStmt {
  struct Statement* statement;
  struct Slice* label;
};

// Keep a payload slot for the empty-statement variant.
struct NullStmt {
  int unused;
};

// Select the concrete statement payload identified by StatementType.
union StatementVariant {
  struct ReturnStmt ret_stmt;
  struct ExprStmt expr_stmt;
  struct IfStmt if_stmt;
  struct GotoStmt goto_stmt;
  struct LabeledStmt labeled_stmt;
  struct CompoundStmt compound_stmt;
  struct BreakStmt break_stmt;
  struct ContinueStmt continue_stmt;
  struct WhileStmt while_stmt;
  struct DoWhileStmt do_while_stmt;
  struct ForStmt for_stmt;
  struct SwitchStmt switch_stmt;
  struct CaseStmt case_stmt;
  struct DefaultStmt default_stmt;
  struct NullStmt null_stmt;
};

// Store statement source location, kind, and payload.
struct Statement {
  char* loc; // start of this statement in the preprocessed source
  union StatementVariant statement;
  enum StatementType type;
};

// Distinguish statement and declaration block items.
enum BlockItemType {
  DCLR_ITEM,
  STMT_ITEM
};

// Select statement or declaration payload within a block item.
union BlockItemVariant {
  struct Statement* stmt;
  struct Declaration* dclr;
};

// Store block-item kind and its selected payload.
struct BlockItem {
  union BlockItemVariant item;
  enum BlockItemType type;
};

// Link block items and track identifiers declared in the block.
struct Block {
  struct BlockItem* item;
  struct IdentMap* idents;
  struct Block* next;
};

// Distinguish ordinary case labels from the default label.
enum CaseLabelType {
  INT_CASE,
  DEFAULT_CASE
};

// Store a case-label kind and its constant/default data.
struct CaseLabel {
  enum CaseLabelType type;
  int data;
};

// Link case labels in a switch statement.
struct CaseList {
  struct CaseLabel case_label;
  struct CaseList* next;
};

// Classify identifier, pointer, function, and array declarators.
enum DeclaratorType {
  IDENT_DEC,
  POINTER_DEC,
  FUN_DEC,
  ARRAY_DEC,
};

// Store the identifier in a direct declarator.
struct IdentDec {
  struct Slice* name;
};

// Store the nested declarator reached through a pointer layer.
struct PointerDec {
  struct Declarator* decl;
};

// Store function parameters and the nested return declarator.
struct FunDec {
  struct ParamInfoList* params;
  struct Declarator* decl;
};

// Store the nested element declarator and array bound.
struct ArrayDec {
  struct Declarator* decl;
  size_t size;
};

// Select the concrete declarator shape identified by DeclaratorType.
union DeclaratorVariant {
  struct IdentDec ident_dec;
  struct PointerDec pointer_dec;
  struct FunDec fun_dec;
  struct ArrayDec array_dec;
};

// Pair a declarator shape with its base type.
struct Declarator {
  enum DeclaratorType type;
  union DeclaratorVariant declarator;
};

// Store a parameter's base type and abstract declarator.
struct ParamInfo {
  struct Type* type;
  struct Declarator decl;
};

// Link parsed parameter descriptors in source order.
struct ParamInfoList {
  struct ParamInfo info;
  struct ParamInfoList* next;
};

// Classify pointer, array, and function abstract-declarator layers.
enum AbstractDeclaratorType {
  ABSTRACT_POINTER,
  ABSTRACT_ARRAY,
  ABSTRACT_FUNCTION,
  ABSTRACT_BASE,
};

// Link the next inner abstract pointer layer.
struct AbstractPointer {
  struct AbstractDeclarator* next;
};

// Store an inner abstract declarator and optional array bound.
struct AbstractArray {
  struct AbstractDeclarator* next;
  size_t size;
};

// Store an inner abstract declarator and function parameter types.
struct AbstractFunction {
  struct AbstractDeclarator* next;
  struct ParamTypeList* params;
};

// Select the concrete abstract-declarator shape.
union AbstractDeclaratorVariant {
  struct AbstractPointer* pointer_type;
  struct AbstractArray* array_type;
  struct AbstractFunction* function_type;
  // no data for AbstractBase
};

// Store an abstract declarator kind and its shape payload.
struct AbstractDeclarator {
  enum AbstractDeclaratorType type;
  union AbstractDeclaratorVariant data;
};

// Classify primitive and tagged type specifiers.
enum TypeSpecifierType {
  INT_SPEC = 1,
  UNSIGNED_SPEC,
  SIGNED_SPEC,
  LONG_SPEC,
  SHORT_SPEC,
  CHAR_SPEC,
  VOID_SPEC,
  STRUCT_SPEC,
  UNION_SPEC,
  ENUM_SPEC,
};

// Store a type-specifier kind and optional named tag.
struct TypeSpecifier {
  enum TypeSpecifierType type;
  struct Slice* name;
};

// Link type specifiers in their source order.
struct TypeSpecList {
  struct TypeSpecifier spec;
  struct TypeSpecList* next;
};

// Link storage-class specifiers in their source order.
struct StorageClassList {
  enum StorageClass spec;
  struct StorageClassList* next;
};

// Distinguish type-specifier and storage-class declaration prefixes.
enum DclrPrefixType {
  STORAGE_PREFIX,
  TYPE_PREFIX
};

// Select type-specifier or storage-class prefix data.
union DclrPrefixVariant {
  struct TypeSpecifier type_spec;
  enum StorageClass storage_class;
};

// Store a declaration-prefix kind and its payload.
struct DclrPrefix {
  enum DclrPrefixType type;
  union DclrPrefixVariant prefix;
};

/*-------------------------------------------------------------------------------------------------------*/

bool compare_types(struct Type* a, struct Type* b);

#endif // AST_H
