#include "../../../config.h"
#include "./arch_config.h"
#include "emit_code.h"

#include <assert.h>
#include <inttypes.h>  // PRId64
#include <stdlib.h>
#include <string.h>

#include "aarch64.h"
#include "ast.h"
#include "codegen.h"
#include "initializer.h"  // calc_bitfield_initial_value
#include "ir.h"
#include "lexer.h"
#include "regalloc.h"
#include "table.h"
#include "type.h"
#include "util.h"
#include "var.h"

char *im(int64_t x) {
  return fmt("#%" PRId64, x);
}

char *immediate_offset(const char *reg, int offset) {
  return offset != 0 ? fmt("[%s,#%d]", reg, offset) : fmt("[%s]", reg);
}

char *pre_index(const char *reg, int offset) {
  return fmt("[%s,#%d]!", reg, offset);
}

char *post_index(const char *reg, int offset) {
  return fmt("[%s],#%d", reg, offset);
}

char *reg_offset(const char *base, const char *reg, const char *shift) {
  if (shift != NULL)
    return fmt("[%s,%s,%s]", base, reg, shift);
  return fmt("[%s,%s]", base, reg);
}

char *label_at_page(char *label, int flag) {
#ifdef __APPLE__
  static const char *s[] = {
    "%s@PAGE", "%s@PAGEOFF",
    "%s@GOTPAGE", "%s@GOTPAGEOFF",
  };
#else
  static const char *s[] = {
    "%s", ":lo12:%s",
    ":got:%s", ":got_lo12:%s",
  };
#endif
  return fmt(s[flag], label);
}

////////

static void eval_initial_value(Expr *expr, Expr **pvar, Fixnum *poffset) {
  switch (expr->kind) {
  case EX_FIXNUM:
    *poffset = expr->fixnum;
    break;
  case EX_VAR:
    assert(*pvar == NULL);
    *pvar = expr;
    break;
  case EX_ADD:
  case EX_SUB:
    {
      Expr *var1 = NULL, *var2 = NULL;
      Fixnum offset1 = 0, offset2 = 0;
      eval_initial_value(expr->bop.lhs, &var1, &offset1);
      eval_initial_value(expr->bop.rhs, &var2, &offset2);
      if (var1 != NULL) {
        assert(var2 == NULL);
        *pvar = var1;
      } else if (var2 != NULL) {
        assert(expr->kind == EX_ADD);
        *pvar = var2;
      }
      if (expr->kind == EX_SUB)
        offset2 = -offset2;
      *poffset = offset1 + offset2;
    }
    break;
  case EX_REF:
  case EX_DEREF:
  case EX_CAST:
    eval_initial_value(expr->unary.sub, pvar, poffset);
    break;
  case EX_MEMBER:
    {
      eval_initial_value(expr->member.target, pvar, poffset);
      const MemberInfo *minfo = expr->member.info;
      *poffset += minfo->offset;
    }
    break;
  case EX_COMPLIT:
    assert(expr->complit.var->kind == EX_VAR);
    eval_initial_value(expr->complit.var, pvar, poffset);
    break;
  // case EX_STR:  // should be handled in parser.
  default: assert(!"illegal"); break;
  }
}

#ifndef __NO_BITFIELD
static int construct_initial_value_bitfield(const StructInfo *sinfo, const Initializer *init,
                                            int start, int *poffset) {
  const MemberInfo *member = &sinfo->members[start];
  if (member->bitfield.width == 0)
    return start;

  const Type *et = get_fixnum_type(member->bitfield.base_kind, false, 0);
  int offset = *poffset;
  int align = align_size(et);
  if (offset % align != 0) {
    EMIT_ALIGN(align);
    offset = ALIGN(offset, align);
  }

  int i = start;
  Fixnum x = calc_bitfield_initial_value(sinfo, init, &i);

  const char *output = NUM(x);
  switch (et->fixnum.kind) {
  case FX_CHAR:  _BYTE(output); break;
  case FX_SHORT: _WORD(output); break;
  case FX_LONG: case FX_LLONG:
    _QUAD(output);
    break;
  case FX_INT: case FX_ENUM:
    _LONG(output);
    break;
  }
  *poffset = offset += type_size(et);

  return i;
}
#endif

static void construct_initial_value(const Type *type, const Initializer *init) {
  assert(init == NULL || init->kind != IK_DOT);

  switch (type->kind) {
  case TY_FLONUM:
#ifndef __NO_FLONUM
    switch (type->flonum.kind) {
    case FL_DOUBLE:
    case FL_LDOUBLE:  // long-double in XCC is same as double.
      {
        union {double f; uint64_t h;} v;
        v.f = 0;
        if (init != NULL) {
          assert(init->kind == IK_SINGLE);
          Expr *value = init->single;
          if (!(is_const(value) && is_flonum(value->type)))
            error("Illegal initializer: constant number expected");
          v.f = value->flonum;
        }
#if 0
        _DOUBLE(FLONUM(v.d));
#else
        _QUAD(HEXNUM(v.h));
#endif
      }
      break;
    case FL_FLOAT:
      {
        union {float f; uint32_t h;} v;
        v.f = 0;
        if (init != NULL) {
          assert(init->kind == IK_SINGLE);
          Expr *value = init->single;
          if (!(is_const(value) && is_flonum(value->type)))
            error("Illegal initializer: constant number expected");
          v.f = value->flonum;
        }
#if 0
        _FLOAT(FLONUM(v.f));
#else
        _LONG(HEXNUM(v.h));
#endif
      }
      break;
    }
#else
    assert(false);
#endif
    break;
  case TY_FIXNUM:
  case TY_PTR:
    {
      Expr *var = NULL;
      Fixnum offset = 0;
      if (init != NULL) {
        assert(init->kind == IK_SINGLE);
        eval_initial_value(init->single, &var, &offset);
      }
      const char *output;
      if (var == NULL) {
        output = NUM(offset);
      } else {
        const Name *name = var->var.name;
        Scope *scope;
        VarInfo *varinfo = scope_find(var->var.scope, name, &scope);
        assert(varinfo != NULL);
        if (!is_global_scope(scope) && varinfo->storage & VS_STATIC) {
          varinfo = varinfo->static_.gvar;
          assert(varinfo != NULL);
          name = varinfo->name;
        }

        char *label = fmt_name(name);
        if ((varinfo->storage & VS_STATIC) == 0)
          label = MANGLE(label);
        label = quote_label(label);

        if (offset == 0) {
          output = label;
        } else {
          output = fmt("%s + %" PRId64, label, offset);
        }
      }
      if (type->kind == TY_PTR) {
        _QUAD(output);
      } else {
        switch (type->fixnum.kind) {
        case FX_CHAR:  _BYTE(output); break;
        case FX_SHORT: _WORD(output); break;
        case FX_LONG: case FX_LLONG:
          _QUAD(output);
          break;
        case FX_INT: case FX_ENUM:
          _LONG(output);
          break;
        }
      }
    }
    break;
  case TY_ARRAY:
    if (init == NULL || init->kind == IK_MULTI) {
      const Type *elem_type = type->pa.ptrof;
      ssize_t index = 0;
      if (init != NULL) {
        Vector *init_array = init->multi;
        for (ssize_t i = 0; i < init_array->len; ++i, ++index) {
          const Initializer *init_elem = init_array->data[i];
          construct_initial_value(elem_type, init_elem);
        }
      }
      // Padding
      for (ssize_t i = index, n = type->pa.length; i < n; ++i)
        construct_initial_value(elem_type, NULL);
      break;
    }
    if (init->kind == IK_SINGLE) {
      Expr *e = strip_cast(init->single);
      if (e->kind == EX_STR && is_char_type(type->pa.ptrof, e->str.kind)) {
        size_t src_size = e->str.len * type_size(e->type->pa.ptrof);
        size_t size = type_size(type);
        if (src_size > size)
          src_size = size;

        UNUSED(size);
        StringBuffer sb;
        sb_init(&sb);
        sb_append(&sb, "\"", NULL);
        escape_string(e->str.buf, src_size, &sb);
        if (size > src_size) {
          const char NULCHR[] = "\\0";
          for (size_t i = 0, n = size - src_size; i < n; ++i)
            sb_append(&sb, NULCHR, NULL);
        }
        sb_append(&sb, "\"", NULL);
        _ASCII(sb_to_string(&sb));
        break;
      }
    }
    error("Illegal initializer");
    break;
  case TY_STRUCT:
    {
      const StructInfo *sinfo = type->struct_.info;
      assert(init == NULL || (init->kind == IK_MULTI && init->multi->len == sinfo->member_count));
      int count = 0;
      int offset = 0;
      for (int i = 0, n = sinfo->member_count; i < n; ++i) {
        const MemberInfo *member = &sinfo->members[i];
#ifndef __NO_BITFIELD
        if (member->bitfield.width >= 0) {
          i = construct_initial_value_bitfield(sinfo, init, i, &offset);
          ++count;
          continue;
        }
#endif
        const Initializer *mem_init;
        if (init == NULL) {
          if (sinfo->is_union)
            continue;
          mem_init = NULL;
        } else {
          mem_init = init->multi->data[i];
        }
        if (mem_init != NULL || !sinfo->is_union) {
          int align = align_size(member->type);
          if (offset % align != 0) {
            EMIT_ALIGN(align);
            offset = ALIGN(offset, align);
          }
          construct_initial_value(member->type, mem_init);
          ++count;
          offset += type_size(member->type);
        }
      }
      if (sinfo->is_union && count <= 0) {
        const MemberInfo *member = &sinfo->members[0];
        construct_initial_value(member->type, NULL);
        offset += type_size(member->type);
      }

      size_t size = type_size(type);
      if (size != (size_t)offset) {
        // Put padding.
        int d = size - offset;
        switch (d) {
        case 1:  _BYTE(NUM(0)); break;
        case 2:  _WORD(NUM(0)); break;
        case 4:  _LONG(NUM(0)); break;
        case 8:  _QUAD(NUM(0)); break;
        default:
          for (int i = 0; i < d; ++i)
            _BYTE(NUM(0));
          break;
        }
      }
    }
    break;
  case TY_FUNC: case TY_VOID: assert(false); break;
  }
}

static void emit_varinfo(const VarInfo *varinfo, const Initializer *init) {
  const Name *name = varinfo->name;
  if (init != NULL) {
    if (varinfo->type->qualifier & TQ_CONST)
      _RODATA();
    else
      _DATA();
  }

  char *label = fmt_name(name);
  if ((varinfo->storage & VS_STATIC) == 0) {  // global
    label = quote_label(MANGLE(label));
    _GLOBL(label);
  } else {
    label = quote_label(label);
    _LOCAL(label);
  }

  if (init != NULL) {
    EMIT_ALIGN(align_size(varinfo->type));
    EMIT_LABEL(label);
    construct_initial_value(varinfo->type, init);
  } else {
    size_t size = type_size(varinfo->type);
    if (size < 1)
      size = 1;

    size_t align = align_size(varinfo->type);
    _BSS(label, size, align);
  }
}

////////////////////////////////////////////////

static bool is_asm(Stmt *stmt) {
  return stmt->kind == ST_ASM;
}

static void move_params_to_assigned(Function *func) {
  extern const char **kRegSizeTable[];
  extern const int ArchRegParamMapping[];
  extern const char *kFReg32s[], *kFReg64s[];

  static const char *kRegParam32s[] = {W0, W1, W2, W3, W4, W5, W6, W7};
  static const char *kRegParam64s[] = {X0, X1, X2, X3, X4, X5, X6, X7};
  static const char **kRegParamTable[] = {kRegParam32s, kRegParam32s, kRegParam32s, kRegParam64s};
  const char *kFRegParam32s[] = {S0, S1, S2, S3, S4, S5, S6, S7};
  const char *kFRegParam64s[] = {D0, D1, D2, D3, D4, D5, D6, D7};
  static const int kPow2Table[] = {-1, 0, 1, -1, 2, -1, -1, -1, 3};
#define kPow2TableSize ((int)(sizeof(kPow2Table) / sizeof(*kPow2Table)))

  RegParamInfo iparams[MAX_REG_ARGS];
  RegParamInfo fparams[MAX_FREG_ARGS];
  int iparam_count = 0;
  int fparam_count = 0;
  enumerate_register_params(func, iparams, MAX_REG_ARGS, fparams, MAX_FREG_ARGS,
                            &iparam_count, &fparam_count);

  // Generate code to store parameters to the destination.
  for (int i = 0; i < iparam_count; ++i) {
    RegParamInfo *p = &iparams[i];
    VReg *vreg = p->vreg;
    size_t size = type_size(p->type);
    assert(0 < size && size < kPow2TableSize && kPow2Table[size] >= 0);
    int pow = kPow2Table[size];
    const char *src = kRegParamTable[pow][p->index];
    if (vreg->flag & VRF_SPILLED) {
      int offset = vreg->frame.offset;
      assert(offset != 0);
      const char *dst;
      if (offset >= -256) {
        dst = IMMEDIATE_OFFSET(FP, offset);
      } else {
        mov_immediate(X9, offset, true, false);  // x9 broken.
        dst = REG_OFFSET(FP, X9, NULL);
      }
      switch (pow) {
      case 0:          STRB(src, dst); break;
      case 1:          STRH(src, dst); break;
      case 2: case 3:  STR(src, dst); break;
      default: assert(false); break;
      }
    } else if (ArchRegParamMapping[p->index] != vreg->phys) {
      const char *dst = kRegSizeTable[pow][vreg->phys];
      MOV(dst, src);
    }
  }
  for (int i = 0; i < fparam_count; ++i) {
    RegParamInfo *p = &fparams[i];
    VReg *vreg = p->vreg;
    const char *src = (p->type->flonum.kind >= FL_DOUBLE ? kFRegParam64s : kFRegParam32s)[p->index];
    if (vreg->flag & VRF_SPILLED) {
      int offset = vreg->frame.offset;
      assert(offset != 0);
      assert(offset != 0);
      STR(src, IMMEDIATE_OFFSET(FP, offset));
    } else {
      if (p->index != vreg->phys) {
        const char *dst = (p->type->flonum.kind >= FL_DOUBLE ? kFReg64s : kFReg32s)[vreg->phys];
        FMOV(dst, src);
      }
    }
  }

#ifdef VAARG_ON_STACK
  bool vaargs = false;
#else
  bool vaargs = func->type->func.vaargs;
#endif
  if (vaargs) {
    for (int i = iparam_count; i < MAX_REG_ARGS; ++i) {
      int offset = (i - MAX_REG_ARGS - MAX_FREG_ARGS) * POINTER_SIZE;
      STR(kRegParam64s[i], IMMEDIATE_OFFSET(FP, offset));
    }
    for (int i = fparam_count; i < MAX_FREG_ARGS; ++i) {
      int offset = (i - MAX_FREG_ARGS) * POINTER_SIZE;
      STR(kFRegParam64s[i], IMMEDIATE_OFFSET(FP, offset));
    }
  }
}

static void emit_defun(Function *func) {
  if (func->scopes == NULL ||  // Prototype definition.
      func->extra == NULL)     // Code emission is omitted.
    return;

  emit_comment(NULL);
  _TEXT();

  bool global = true;
  const VarInfo *varinfo = scope_find(global_scope, func->name, NULL);
  if (varinfo != NULL) {
    global = (varinfo->storage & VS_STATIC) == 0;
  }

  char *label = fmt_name(func->name);
  if (global) {
    label = quote_label(MANGLE(label));
    _GLOBL(label);
  } else {
    emit_comment("%.*s: static func", NAMES(func->name));
    label = quote_label(label);
    _LOCAL(label);
  }
  EMIT_ALIGN(4);
  EMIT_LABEL(label);

  bool no_stmt = true;
  if (func->body_block != NULL) {
    Vector *stmts = func->body_block->block.stmts;
    for (int i = 0; i < stmts->len; ++i) {
      Stmt *stmt = stmts->data[i];
      if (stmt == NULL)
        continue;
      if (!is_asm(stmt)) {
        no_stmt = false;
        break;
      }
    }
  }

  // Prologue
  // Allocate variable bufer.
  FuncBackend *fnbe = func->extra;
  size_t frame_size = ALIGN(fnbe->frame_size, 16);
  bool fp_saved = false;  // Frame pointer saved?
  bool lr_saved = false;  // Link register saved?
  unsigned long used_reg_bits = fnbe->ra->used_reg_bits;
  if (!no_stmt) {
    fp_saved = frame_size > 0 || fnbe->ra->flag & RAF_STACK_FRAME;
    lr_saved = (func->flag & FUNCF_HAS_FUNCALL) != 0;

    // TODO: Handle fp_saved and lr_saved individually.
    if (fp_saved || lr_saved) {
      STP(FP, LR, PRE_INDEX(SP, -16));

      // FP is saved, so omit from callee save.
      used_reg_bits &= ~(1UL << GET_FPREG_INDEX());
    }

    // Callee save.
    push_callee_save_regs(used_reg_bits, fnbe->ra->used_freg_bits);

    if (fp_saved) {
      MOV(FP, SP);
      if (frame_size > 0) {
        const char *value;
        if (frame_size <= 0x0fff) {
          value = IM(frame_size);
        } else {
          // Break x17
          mov_immediate(value = X17, frame_size, true, false);
        }
        SUB(SP, SP, value);
      }
    }

    move_params_to_assigned(func);
  }

  emit_bb_irs(fnbe->bbcon);

  if (!function_not_returned(fnbe)) {
    // Epilogue
    if (!no_stmt) {
      if (fp_saved)
        MOV(SP, FP);

      pop_callee_save_regs(used_reg_bits, fnbe->ra->used_freg_bits);

      if (fp_saved || lr_saved)
        LDP(FP, LR, POST_INDEX(SP, 16));
    }

    RET();
  }

  // Static variables are emitted through global variables.
}

static void emit_asm(Expr *asmstr) {
  assert(asmstr->kind == EX_STR);
  EMIT_ASM(asmstr->str.buf);
}

void emit_code(Vector *decls) {
  for (int i = 0, len = decls->len; i < len; ++i) {
    Declaration *decl = decls->data[i];
    if (decl == NULL)
      continue;

    switch (decl->kind) {
    case DCL_DEFUN:
      emit_defun(decl->defun.func);
      break;
    case DCL_VARDECL:
      break;
    case DCL_ASM:
      emit_asm(decl->asmstr);
      break;
    }
  }

  emit_comment(NULL);
  for (int i = 0; i < global_scope->vars->len; ++i) {
    VarInfo *varinfo = global_scope->vars->data[i];
    if ((varinfo->storage & (VS_EXTERN | VS_ENUM_MEMBER)) || varinfo->type->kind == TY_FUNC)
      continue;
    emit_varinfo(varinfo, varinfo->global.init);
  }
}
