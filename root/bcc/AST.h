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

struct ParamTypeList {
  struct Type* type;
  struct ParamTypeList* next;
};

enum DclrType {
  VAR_DCLR,
  FUN_DCLR,
  STRUCT_DCLR,
  UNION_DCLR,
  ENUM_DCLR,
  MEMBER_DCLR,
};

enum InitializerType {
  SINGLE_INIT,
  COMPOUND_INIT,
};

struct InitializerList {
  struct Initializer* init;
  struct InitializerList* next;
};

union InitializerVariant {
  struct Expr* single_init;
  struct InitializerList* compound_init;
};

struct Initializer {
  enum InitializerType init_type;
  union InitializerVariant init;
  struct Type* type; // target type for this initializer
  char* loc; // location in source for error reporting
};

struct VarAttributes {
  struct Slice* cleanup_func;
};

struct VariableDclr {
  struct Slice* name;
  struct Initializer* init;
  struct Type* type;
  enum StorageClass storage;
  struct VarAttributes attributes;
};

struct FunctionDclr {
  struct Slice* name;
  enum StorageClass storage;
  struct ParamList* params;
  struct Type* type;
  struct Block* body;
};

struct MemberDclr {
  struct Slice* name;
  struct Type* type;
  struct MemberDclr* next;
};

struct EnumMemberDclr {
  struct Slice* name;
  int value;
  struct EnumMemberDclr* next;
};

struct StructDclr {
  struct Slice* name;
  struct MemberDclr* members;
};

struct UnionDclr {
  struct Slice* name;
  struct MemberDclr* members;
};

struct EnumDclr {
  struct Slice* name;
  struct EnumMemberDclr* members;
};

union DeclareVariant {
  struct VariableDclr var_dclr;
  struct FunctionDclr fun_dclr;
  struct StructDclr struct_dclr;
  struct UnionDclr union_dclr;
  struct EnumDclr enum_dclr;
  struct MemberDclr member_dclr;
};

struct ParamList {
  struct VariableDclr param;
  struct ParamList* next;
};

struct Declaration {
  union DeclareVariant dclr;
  enum DclrType type;
};

struct DeclarationList {
  struct Declaration dclr;
  struct DeclarationList* next;
};

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

enum UnOp {
  COMPLEMENT = 1,
  NEGATE,
  BOOL_NOT,
  UNARY_PLUS,
};

struct BinaryExpr {
  enum BinOp op;
  struct Expr* left;
  struct Expr* right;
};

struct AssignExpr {
  struct Expr* left;
  struct Expr* right;
};

enum PostOp {
  POST_INC,
  POST_DEC
};

struct PostAssignExpr {
  enum PostOp op;
  struct Expr* expr;
};

struct ConditionalExpr {
  struct Expr* condition;
  struct Expr* left;
  struct Expr* right;
};

enum ConstType {
  INT_CONST,
  UINT_CONST,
  LONG_CONST,
  ULONG_CONST
};

union ConstVariant {
  char char_val;
  short short_val;
  unsigned short ushort_val;
  int int_val;
  unsigned uint_val;
  long long_val;
  unsigned long ulong_val;
};

struct LitExpr {
  enum ConstType type;
  union ConstVariant value;
};

struct UnaryExpr {
  enum UnOp op;
  struct Expr* expr;
};

struct VarExpr {
  struct Slice* name;
};

struct FunctionCallExpr {
  struct Expr* func; // name or pointer
  struct ArgList* args;
};

struct CastExpr {
  struct Type* target;
  struct Expr* expr;
};

struct AddrOfExpr {
  struct Expr* expr;
};

struct DereferenceExpr {
  struct Expr* expr;
};

struct SubscriptExpr {
  struct Expr* array;
  struct Expr* index;
};

struct StringExpr {
  struct Slice* string;
};

struct SizeOfExpr {
  struct Expr* expr;
};

struct SizeOfTExpr {
  struct Type* type;
};

struct StmtExpr {
  struct Block* block;
};

struct DotExpr {
  struct Expr* struct_expr;
  struct Slice* member;
};

struct ArrowExpr {
  struct Expr* pointer_expr;
  struct Slice* member;
};

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

struct Expr {
  char* loc; // start of this expression in the preprocessed source
  struct Type* value_type;
  enum ExprType type;
  union ExprVariant expr;
};

struct ArgList {
  struct Expr* arg;
  struct ArgList* next;
};

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

struct ReturnStmt {
  struct Expr* expr;
  struct Slice* func;
};

struct ExprStmt {
  struct Expr* expr;
};

struct IfStmt {
  struct Expr* condition;
  struct Statement* if_stmt;
  struct Statement* else_stmt;
};

struct GotoStmt {
  struct Slice* label;
};

struct LabeledStmt {
  struct Slice* label;
  struct Statement* stmt;
};

struct CompoundStmt {
  struct Block* block;
};

struct BreakStmt {
  struct Slice* label;
};

struct ContinueStmt {
  struct Slice* label;
};

struct WhileStmt {
  struct Expr* condition;
  struct Statement* statement;
  struct Slice* label;
};

struct DoWhileStmt {
  struct Statement* statement;
  struct Expr* condition;
  struct Slice* label;
};

struct ForStmt {
  struct ForInit* init;
  struct Expr* condition;
  struct Expr* end;
  struct Statement* statement;
  struct Slice* label;
  struct IdentMap* init_idents;
};

enum ForInitType {
  DCLR_INIT,
  EXPR_INIT,
};  

union ForInitVariant {
  struct VariableDclr* dclr_init;
  struct Expr* expr_init;
};

struct ForInit {
  enum ForInitType type;
  union ForInitVariant init;
};

struct SwitchStmt {
  struct Expr* condition;
  struct Statement* statement;
  struct Slice* label;
  struct CaseList* cases;
};

struct CaseStmt {
  struct Expr* expr;
  struct Statement* statement;
  struct Slice* label;
};

struct DefaultStmt {
  struct Statement* statement;
  struct Slice* label;
};

struct NullStmt {
  int unused;
};

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

struct Statement {
  char* loc; // start of this statement in the preprocessed source
  union StatementVariant statement;
  enum StatementType type;
};

enum BlockItemType {
  DCLR_ITEM,
  STMT_ITEM
};

union BlockItemVariant {
  struct Statement* stmt;
  struct Declaration* dclr;
};

struct BlockItem {
  union BlockItemVariant item;
  enum BlockItemType type;
};

struct Block {
  struct BlockItem* item;
  struct IdentMap* idents;
  struct Block* next;
};

enum CaseLabelType {
  INT_CASE,
  DEFAULT_CASE
};

struct CaseLabel {
  enum CaseLabelType type;
  int data;
};

struct CaseList {
  struct CaseLabel case_label;
  struct CaseList* next;
};

enum DeclaratorType {
  IDENT_DEC,
  POINTER_DEC,
  FUN_DEC,
  ARRAY_DEC,
};

struct IdentDec {
  struct Slice* name;
};

struct PointerDec {
  struct Declarator* decl;
};

struct FunDec {
  struct ParamInfoList* params;
  struct Declarator* decl;
};

struct ArrayDec {
  struct Declarator* decl;
  size_t size;
};

union DeclaratorVariant {
  struct IdentDec ident_dec;
  struct PointerDec pointer_dec;
  struct FunDec fun_dec;
  struct ArrayDec array_dec;
};

struct Declarator {
  enum DeclaratorType type;
  union DeclaratorVariant declarator;
};

struct ParamInfo {
  struct Type* type;
  struct Declarator decl;
};

struct ParamInfoList {
  struct ParamInfo info;
  struct ParamInfoList* next;
};

enum AbstractDeclaratorType {
  ABSTRACT_POINTER,
  ABSTRACT_ARRAY,
  ABSTRACT_FUNCTION,
  ABSTRACT_BASE,
};

struct AbstractPointer {
  struct AbstractDeclarator* next;
};

struct AbstractArray {
  struct AbstractDeclarator* next;
  size_t size;
};

struct AbstractFunction {
  struct AbstractDeclarator* next;
  struct ParamTypeList* params;
};

union AbstractDeclaratorVariant {
  struct AbstractPointer* pointer_type;
  struct AbstractArray* array_type;
  struct AbstractFunction* function_type;
  // no data for AbstractBase
};

struct AbstractDeclarator {
  enum AbstractDeclaratorType type;
  union AbstractDeclaratorVariant data;
};

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

struct TypeSpecifier {
  enum TypeSpecifierType type;
  struct Slice* name;
};

struct TypeSpecList {
  struct TypeSpecifier spec;
  struct TypeSpecList* next;
};

struct StorageClassList {
  enum StorageClass spec;
  struct StorageClassList* next;
};

enum DclrPrefixType {
  STORAGE_PREFIX,
  TYPE_PREFIX
};

union DclrPrefixVariant {
  struct TypeSpecifier type_spec;
  enum StorageClass storage_class;
};

struct DclrPrefix {
  enum DclrPrefixType type;
  union DclrPrefixVariant prefix;
};

/*-------------------------------------------------------------------------------------------------------*/

bool compare_types(struct Type* a, struct Type* b);

#endif // AST_H
