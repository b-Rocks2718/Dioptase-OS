#include "../crt/print.h"

#include "AST.h"

bool compare_types(struct Type* a, struct Type* b) {
  if (a->type != b->type) {
    return false;
  }

  switch (a->type) {
    case SHORT_TYPE:
    case USHORT_TYPE:
    case INT_TYPE:
    case UINT_TYPE:
    case LONG_TYPE:
    case ULONG_TYPE:
    case CHAR_TYPE:
    case SCHAR_TYPE:
    case UCHAR_TYPE:
    case VOID_TYPE:
      return true;

    case POINTER_TYPE:
      return compare_types(a->type_data.pointer_type.referenced_type,
                           b->type_data.pointer_type.referenced_type);

    case ARRAY_TYPE: {
      struct ArrayType* arr_a = &a->type_data.array_type;
      struct ArrayType* arr_b = &b->type_data.array_type;
      if (arr_a->size != arr_b->size) {
        return false;
      }
      return compare_types(arr_a->element_type, arr_b->element_type);
    }

    case FUN_TYPE: {
      struct FunType* fun_a = &a->type_data.fun_type;
      struct FunType* fun_b = &b->type_data.fun_type;
      struct ParamTypeList* param_a;
      struct ParamTypeList* param_b;

      if (!compare_types(fun_a->return_type, fun_b->return_type)) {
        return false;
      }

      param_a = fun_a->param_types;
      param_b = fun_b->param_types;
      while (param_a != NULL && param_b != NULL) {
        if (!compare_types(param_a->type, param_b->type)) {
          return false;
        }
        param_a = param_a->next;
        param_b = param_b->next;
      }
      return param_a == NULL && param_b == NULL;
    }

    case STRUCT_TYPE:
      return compare_slice_to_slice(a->type_data.struct_type.name,
                                    b->type_data.struct_type.name);
    case UNION_TYPE:
      return compare_slice_to_slice(a->type_data.union_type.name,
                                    b->type_data.union_type.name);
    case ENUM_TYPE:
      return compare_slice_to_slice(a->type_data.enum_type.name,
                                    b->type_data.enum_type.name);
    default:
      puts("Type error: unknown type in compare_types\n");
      return false;
  }
}
