#ifndef TYPES_H
#define TYPES_H

#include "../crt/stddef.h"

struct ParamTypeList;
struct Slice;
struct Type;

enum StorageClass {
  NONE,
  STATIC,
  EXTERN
};

enum TypeType {
  CHAR_TYPE,
  SCHAR_TYPE,
  UCHAR_TYPE,
  INT_TYPE,
  LONG_TYPE,
  SHORT_TYPE,
  UINT_TYPE,
  ULONG_TYPE,
  USHORT_TYPE,
  FUN_TYPE,
  POINTER_TYPE,
  ARRAY_TYPE,
  VOID_TYPE,
  STRUCT_TYPE,
  UNION_TYPE,
  ENUM_TYPE,
};

struct FunType {
  struct ParamTypeList* param_types;
  struct Type* return_type;
};

struct PointerType {
  struct Type* referenced_type;
};

struct ArrayType {
  struct Type* element_type;
  size_t size;
};

struct StructType {
  struct Slice* name;
};

struct UnionType {
  struct Slice* name;
};

struct EnumType {
  struct Slice* name;
};

union TypeVariant {
  struct FunType fun_type;
  struct PointerType pointer_type;
  struct ArrayType array_type;
  struct StructType struct_type;
  struct UnionType union_type;
  struct EnumType enum_type;
  // no data for other types
};

struct Type {
  enum TypeType type;
  union TypeVariant type_data;
};

#endif // TYPES_H
