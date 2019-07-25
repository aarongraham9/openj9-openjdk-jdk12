/*
 * Copyright © 2018 Adobe Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Adobe Author(s): Michiharu Ariza
 */

#ifndef HB_SUBSET_CFF_COMMON_HH
#define HB_SUBSET_CFF_COMMON_HH

#include "hb.hh"

#include "hb-subset-plan.hh"
#include "hb-cff-interp-cs-common.hh"

namespace CFF {

/* Used for writing a temporary charstring */
struct str_encoder_t
{
  str_encoder_t (str_buff_t &buff_)
    : buff (buff_), error (false) {}

  void reset () { buff.resize (0); }

  void encode_byte (unsigned char b)
  {
    if (unlikely (buff.push (b) == &Crap(unsigned char)))
      set_error ();
  }

  void encode_int (int v)
  {
    if ((-1131 <= v) && (v <= 1131))
    {
      if ((-107 <= v) && (v <= 107))
        encode_byte (v + 139);
      else if (v > 0)
      {
        v -= 108;
        encode_byte ((v >> 8) + OpCode_TwoBytePosInt0);
        encode_byte (v & 0xFF);
      }
      else
      {
        v = -v - 108;
        encode_byte ((v >> 8) + OpCode_TwoByteNegInt0);
        encode_byte (v & 0xFF);
      }
    }
    else
    {
      if (unlikely (v < -32768))
        v = -32768;
      else if (unlikely (v > 32767))
        v = 32767;
      encode_byte (OpCode_shortint);
      encode_byte ((v >> 8) & 0xFF);
      encode_byte (v & 0xFF);
    }
  }

  void encode_num (const number_t& n)
  {
    if (n.in_int_range ())
    {
      encode_int (n.to_int ());
    }
    else
    {
      int32_t v = n.to_fixed ();
      encode_byte (OpCode_fixedcs);
      encode_byte ((v >> 24) & 0xFF);
      encode_byte ((v >> 16) & 0xFF);
      encode_byte ((v >> 8) & 0xFF);
      encode_byte (v & 0xFF);
    }
  }

  void encode_op (op_code_t op)
  {
    if (Is_OpCode_ESC (op))
    {
      encode_byte (OpCode_escape);
      encode_byte (Unmake_OpCode_ESC (op));
    }
    else
      encode_byte (op);
  }

  void copy_str (const byte_str_t &str)
  {
    unsigned int  offset = buff.length;
    buff.resize (offset + str.length);
    if (unlikely (buff.length < offset + str.length))
    {
      set_error ();
      return;
    }
    memcpy (&buff[offset], &str[0], str.length);
  }

  bool is_error () const { return error; }

  protected:
  void set_error () { error = true; }

  str_buff_t &buff;
  bool    error;
};

struct cff_sub_table_offsets_t {
  cff_sub_table_offsets_t () : privateDictsOffset (0)
  {
    topDictInfo.init ();
    FDSelectInfo.init ();
    FDArrayInfo.init ();
    charStringsInfo.init ();
    globalSubrsInfo.init ();
    localSubrsInfos.init ();
  }

  ~cff_sub_table_offsets_t () { localSubrsInfos.fini (); }

  table_info_t     topDictInfo;
  table_info_t     FDSelectInfo;
  table_info_t     FDArrayInfo;
  table_info_t     charStringsInfo;
  unsigned int  privateDictsOffset;
  table_info_t     globalSubrsInfo;
  hb_vector_t<table_info_t>  localSubrsInfos;
};

template <typename OPSTR=op_str_t>
struct cff_top_dict_op_serializer_t : op_serializer_t
{
  bool serialize (hb_serialize_context_t *c,
                  const OPSTR &opstr,
                  const cff_sub_table_offsets_t &offsets) const
  {
    TRACE_SERIALIZE (this);

    switch (opstr.op)
    {
      case OpCode_CharStrings:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.charStringsInfo.offset));

      case OpCode_FDArray:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.FDArrayInfo.offset));

      case OpCode_FDSelect:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.FDSelectInfo.offset));

      default:
        return_trace (copy_opstr (c, opstr));
    }
    return_trace (true);
  }

  unsigned int calculate_serialized_size (const OPSTR &opstr) const
  {
    switch (opstr.op)
    {
      case OpCode_CharStrings:
      case OpCode_FDArray:
      case OpCode_FDSelect:
        return OpCode_Size (OpCode_longintdict) + 4 + OpCode_Size (opstr.op);

      default:
        return opstr.str.length;
    }
  }
};

struct cff_font_dict_op_serializer_t : op_serializer_t
{
  bool serialize (hb_serialize_context_t *c,
                  const op_str_t &opstr,
                  const table_info_t &privateDictInfo) const
  {
    TRACE_SERIALIZE (this);

    if (opstr.op == OpCode_Private)
    {
      /* serialize the private dict size & offset as 2-byte & 4-byte integers */
      if (unlikely (!UnsizedByteStr::serialize_int2 (c, privateDictInfo.size) ||
                    !UnsizedByteStr::serialize_int4 (c, privateDictInfo.offset)))
        return_trace (false);

      /* serialize the opcode */
      HBUINT8 *p = c->allocate_size<HBUINT8> (1);
      if (unlikely (p == nullptr)) return_trace (false);
      p->set (OpCode_Private);

      return_trace (true);
    }
    else
    {
      HBUINT8 *d = c->allocate_size<HBUINT8> (opstr.str.length);
      if (unlikely (d == nullptr)) return_trace (false);
      memcpy (d, &opstr.str[0], opstr.str.length);
    }
    return_trace (true);
  }

  unsigned int calculate_serialized_size (const op_str_t &opstr) const
  {
    if (opstr.op == OpCode_Private)
      return OpCode_Size (OpCode_longintdict) + 4 + OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (OpCode_Private);
    else
      return opstr.str.length;
  }
};

struct cff_private_dict_op_serializer_t : op_serializer_t
{
  cff_private_dict_op_serializer_t (bool desubroutinize_, bool drop_hints_)
    : desubroutinize (desubroutinize_), drop_hints (drop_hints_) {}

  bool serialize (hb_serialize_context_t *c,
                  const op_str_t &opstr,
                  const unsigned int subrsOffset) const
  {
    TRACE_SERIALIZE (this);

    if (drop_hints && dict_opset_t::is_hint_op (opstr.op))
      return true;
    if (opstr.op == OpCode_Subrs)
    {
      if (desubroutinize || (subrsOffset == 0))
        return_trace (true);
      else
        return_trace (FontDict::serialize_offset2_op (c, opstr.op, subrsOffset));
    }
    else
      return_trace (copy_opstr (c, opstr));
  }

  unsigned int calculate_serialized_size (const op_str_t &opstr,
                                          bool has_localsubr=true) const
  {
    if (drop_hints && dict_opset_t::is_hint_op (opstr.op))
      return 0;
    if (opstr.op == OpCode_Subrs)
    {
      if (desubroutinize || !has_localsubr)
        return 0;
      else
        return OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (opstr.op);
    }
    else
      return opstr.str.length;
  }

  protected:
  const bool  desubroutinize;
  const bool  drop_hints;
};

struct flatten_param_t
{
  str_buff_t     &flatStr;
  bool  drop_hints;

  // Solaris: OS12u4 complains about "A class with a reference member lacks a user-defined constructor"
  // so provide the constructor
  flatten_param_t(str_buff_t& sbt, bool dh) : flatStr(sbt), drop_hints(dh) {}
};

template <typename ACC, typename ENV, typename OPSET>
struct subr_flattener_t
{
  subr_flattener_t (const ACC &acc_,
                    const hb_vector_t<hb_codepoint_t> &glyphs_,
                    bool drop_hints_) : acc (acc_), glyphs (glyphs_),
                                        drop_hints (drop_hints_) {}

  bool flatten (str_buff_vec_t &flat_charstrings)
  {
    if (!flat_charstrings.resize (glyphs.length))
      return false;
    for (unsigned int i = 0; i < glyphs.length; i++)
      flat_charstrings[i].init ();
    for (unsigned int i = 0; i < glyphs.length; i++)
    {
      hb_codepoint_t  glyph = glyphs[i];
      const byte_str_t str = (*acc.charStrings)[glyph];
      unsigned int fd = acc.fdSelect->get_fd (glyph);
      if (unlikely (fd >= acc.fdCount))
        return false;
      cs_interpreter_t<ENV, OPSET, flatten_param_t> interp;
      interp.env.init (str, acc, fd);
      // Solaris: OS12u4 does not like the C++11 style init
      // flatten_param_t  param = { flat_charstrings[i], drop_hints };
      flatten_param_t  param(flat_charstrings[i], drop_hints);
      if (unlikely (!interp.interpret (param)))
        return false;
    }
    return true;
  }

  const ACC &acc;
  const hb_vector_t<hb_codepoint_t> &glyphs;
  bool  drop_hints;
};

struct subr_closures_t
{
  subr_closures_t () : valid (false), global_closure (nullptr)
  { local_closures.init (); }

  void init (unsigned int fd_count)
  {
    valid = true;
    global_closure = hb_set_create ();
    if (global_closure == hb_set_get_empty ())
      valid = false;
    if (!local_closures.resize (fd_count))
      valid = false;

    for (unsigned int i = 0; i < local_closures.length; i++)
    {
      local_closures[i] = hb_set_create ();
      if (local_closures[i] == hb_set_get_empty ())
        valid = false;
    }
  }

  void fini ()
  {
    hb_set_destroy (global_closure);
    for (unsigned int i = 0; i < local_closures.length; i++)
      hb_set_destroy (local_closures[i]);
    local_closures.fini ();
  }

  void reset ()
  {
    hb_set_clear (global_closure);
    for (unsigned int i = 0; i < local_closures.length; i++)
      hb_set_clear (local_closures[i]);
  }

  bool is_valid () const { return valid; }
  bool  valid;
  hb_set_t  *global_closure;
  hb_vector_t<hb_set_t *> local_closures;
};

struct parsed_cs_op_t : op_str_t
{
  void init (unsigned int subr_num_ = 0)
  {
    op_str_t::init ();
    subr_num = subr_num_;
    drop_flag = false;
    keep_flag = false;
    skip_flag = false;
  }

  void fini () { op_str_t::fini (); }

  bool for_drop () const { return drop_flag; }
  void set_drop ()       { if (!for_keep ()) drop_flag = true; }

  bool for_keep () const { return keep_flag; }
  void set_keep ()       { keep_flag = true; }

  bool for_skip () const { return skip_flag; }
  void set_skip ()       { skip_flag = true; }

  unsigned int  subr_num;

  protected:
  bool    drop_flag : 1;
  bool    keep_flag : 1;
  bool    skip_flag : 1;
};

struct parsed_cs_str_t : parsed_values_t<parsed_cs_op_t>
{
  void init ()
  {
    SUPER::init ();
    parsed = false;
    hint_dropped = false;
    has_prefix_ = false;
  }

  void add_op (op_code_t op, const byte_str_ref_t& str_ref)
  {
    if (!is_parsed ())
      SUPER::add_op (op, str_ref);
  }

  void add_call_op (op_code_t op, const byte_str_ref_t& str_ref, unsigned int subr_num)
  {
    if (!is_parsed ())
    {
      unsigned int parsed_len = get_count ();
      if (likely (parsed_len > 0))
        values[parsed_len-1].set_skip ();

      parsed_cs_op_t val;
      val.init (subr_num);
      SUPER::add_op (op, str_ref, val);
    }
  }

  void set_prefix (const number_t &num, op_code_t op = OpCode_Invalid)
  {
    has_prefix_ = true;
    prefix_op_ = op;
    prefix_num_ = num;
  }

  bool at_end (unsigned int pos) const
  {
    return ((pos + 1 >= values.length) /* CFF2 */
        || (values[pos + 1].op == OpCode_return));
  }

  bool is_parsed () const { return parsed; }
  void set_parsed ()      { parsed = true; }

  bool is_hint_dropped () const { return hint_dropped; }
  void set_hint_dropped ()      { hint_dropped = true; }

  bool is_vsindex_dropped () const { return vsindex_dropped; }
  void set_vsindex_dropped ()      { vsindex_dropped = true; }

  bool has_prefix () const          { return has_prefix_; }
  op_code_t prefix_op () const         { return prefix_op_; }
  const number_t &prefix_num () const { return prefix_num_; }

  protected:
  bool    parsed;
  bool    hint_dropped;
  bool    vsindex_dropped;
  bool    has_prefix_;
  op_code_t     prefix_op_;
  number_t      prefix_num_;

  private:
  typedef parsed_values_t<parsed_cs_op_t> SUPER;
};

struct parsed_cs_str_vec_t : hb_vector_t<parsed_cs_str_t>
{
  void init (unsigned int len_ = 0)
  {
    SUPER::init ();
    resize (len_);
    for (unsigned int i = 0; i < length; i++)
      (*this)[i].init ();
  }
  void fini () { SUPER::fini_deep (); }

  private:
  typedef hb_vector_t<parsed_cs_str_t> SUPER;
};

struct subr_subset_param_t
{
  void init (parsed_cs_str_t *parsed_charstring_,
             parsed_cs_str_vec_t *parsed_global_subrs_, parsed_cs_str_vec_t *parsed_local_subrs_,
             hb_set_t *global_closure_, hb_set_t *local_closure_,
             bool drop_hints_)
  {
    parsed_charstring = parsed_charstring_;
    current_parsed_str = parsed_charstring;
    parsed_global_subrs = parsed_global_subrs_;
    parsed_local_subrs = parsed_local_subrs_;
    global_closure = global_closure_;
    local_closure = local_closure_;
    drop_hints = drop_hints_;
  }

  parsed_cs_str_t *get_parsed_str_for_context (call_context_t &context)
  {
    switch (context.type)
    {
      case CSType_CharString:
        return parsed_charstring;

      case CSType_LocalSubr:
        if (likely (context.subr_num < parsed_local_subrs->length))
          return &(*parsed_local_subrs)[context.subr_num];
        break;

      case CSType_GlobalSubr:
        if (likely (context.subr_num < parsed_global_subrs->length))
          return &(*parsed_global_subrs)[context.subr_num];
        break;
    }
    return nullptr;
  }

  template <typename ENV>
  void set_current_str (ENV &env, bool calling)
  {
    parsed_cs_str_t  *parsed_str = get_parsed_str_for_context (env.context);
    if (likely (parsed_str != nullptr))
    {
      /* If the called subroutine is parsed partially but not completely yet,
       * it must be because we are calling it recursively.
       * Handle it as an error. */
      if (unlikely (calling && !parsed_str->is_parsed () && (parsed_str->values.length > 0)))
        env.set_error ();
      else
        current_parsed_str = parsed_str;
    }
    else
      env.set_error ();
  }

  parsed_cs_str_t       *current_parsed_str;

  parsed_cs_str_t       *parsed_charstring;
  parsed_cs_str_vec_t   *parsed_global_subrs;
  parsed_cs_str_vec_t   *parsed_local_subrs;
  hb_set_t      *global_closure;
  hb_set_t      *local_closure;
  bool    drop_hints;
};

struct subr_remap_t : remap_t
{
  void create (hb_set_t *closure)
  {
    /* create a remapping of subroutine numbers from old to new.
     * no optimization based on usage counts. fonttools doesn't appear doing that either.
     */
    reset (closure->get_max () + 1);
    for (hb_codepoint_t old_num = 0; old_num < length; old_num++)
    {
      if (hb_set_has (closure, old_num))
        add (old_num);
    }

    if (get_count () < 1240)
      bias = 107;
    else if (get_count () < 33900)
      bias = 1131;
    else
      bias = 32768;
  }

  hb_codepoint_t operator[] (unsigned int old_num) const
  {
    if (old_num >= length)
      return CFF_UNDEF_CODE;
    else
      return remap_t::operator[] (old_num);
  }

  int biased_num (unsigned int old_num) const
  {
    hb_codepoint_t new_num = (*this)[old_num];
    return (int)new_num - bias;
  }

  protected:
  int bias;
};

struct subr_remap_ts
{
  subr_remap_ts ()
  {
    global_remap.init ();
    local_remaps.init ();
  }

  ~subr_remap_ts () { fini (); }

  void init (unsigned int fdCount)
  {
    local_remaps.resize (fdCount);
    for (unsigned int i = 0; i < fdCount; i++)
      local_remaps[i].init ();
  }

  void create (subr_closures_t& closures)
  {
    global_remap.create (closures.global_closure);
    for (unsigned int i = 0; i < local_remaps.length; i++)
      local_remaps[i].create (closures.local_closures[i]);
  }

  void fini ()
  {
    global_remap.fini ();
    local_remaps.fini_deep ();
  }

  subr_remap_t         global_remap;
  hb_vector_t<subr_remap_t>  local_remaps;
};

template <typename SUBSETTER, typename SUBRS, typename ACC, typename ENV, typename OPSET>
struct subr_subsetter_t
{
  subr_subsetter_t ()
  {
    parsed_charstrings.init ();
    parsed_global_subrs.init ();
    parsed_local_subrs.init ();
  }

  ~subr_subsetter_t ()
  {
    closures.fini ();
    remaps.fini ();
    parsed_charstrings.fini_deep ();
    parsed_global_subrs.fini_deep ();
    parsed_local_subrs.fini_deep ();
  }

  /* Subroutine subsetting with --no-desubroutinize runs in phases:
   *
   * 1. execute charstrings/subroutines to determine subroutine closures
   * 2. parse out all operators and numbers
   * 3. mark hint operators and operands for removal if --no-hinting
   * 4. re-encode all charstrings and subroutines with new subroutine numbers
   *
   * Phases #1 and #2 are done at the same time in collect_subrs ().
   * Phase #3 walks charstrings/subroutines forward then backward (hence parsing required),
   * because we can't tell if a number belongs to a hint op until we see the first moveto.
   *
   * Assumption: a callsubr/callgsubr operator must immediately follow a (biased) subroutine number
   * within the same charstring/subroutine, e.g., not split across a charstring and a subroutine.
   */
  bool subset (ACC &acc, const hb_vector_t<hb_codepoint_t> &glyphs, bool drop_hints)
  {
    closures.init (acc.fdCount);
    remaps.init (acc.fdCount);

    parsed_charstrings.init (glyphs.length);
    parsed_global_subrs.init (acc.globalSubrs->count);
    parsed_local_subrs.resize (acc.fdCount);
    for (unsigned int i = 0; i < acc.fdCount; i++)
    {
      parsed_local_subrs[i].init (acc.privateDicts[i].localSubrs->count);
    }
    if (unlikely (!closures.valid))
      return false;

    /* phase 1 & 2 */
    for (unsigned int i = 0; i < glyphs.length; i++)
    {
      hb_codepoint_t  glyph = glyphs[i];
      const byte_str_t str = (*acc.charStrings)[glyph];
      unsigned int fd = acc.fdSelect->get_fd (glyph);
      if (unlikely (fd >= acc.fdCount))
        return false;

      cs_interpreter_t<ENV, OPSET, subr_subset_param_t> interp;
      interp.env.init (str, acc, fd);

      subr_subset_param_t  param;
      param.init (&parsed_charstrings[i],
                  &parsed_global_subrs,  &parsed_local_subrs[fd],
                  closures.global_closure, closures.local_closures[fd],
                  drop_hints);

      if (unlikely (!interp.interpret (param)))
        return false;

      /* finalize parsed string esp. copy CFF1 width or CFF2 vsindex to the parsed charstring for encoding */
      SUBSETTER::finalize_parsed_str (interp.env, param, parsed_charstrings[i]);
    }

    if (drop_hints)
    {
      /* mark hint ops and arguments for drop */
      for (unsigned int i = 0; i < glyphs.length; i++)
      {
        unsigned int fd = acc.fdSelect->get_fd (glyphs[i]);
        if (unlikely (fd >= acc.fdCount))
          return false;
        subr_subset_param_t  param;
        param.init (&parsed_charstrings[i],
                    &parsed_global_subrs,  &parsed_local_subrs[fd],
                    closures.global_closure, closures.local_closures[fd],
                    drop_hints);

        drop_hints_param_t  drop;
        if (drop_hints_in_str (parsed_charstrings[i], param, drop))
        {
          parsed_charstrings[i].set_hint_dropped ();
          if (drop.vsindex_dropped)
            parsed_charstrings[i].set_vsindex_dropped ();
        }
      }

      /* after dropping hints recreate closures of actually used subrs */
      closures.reset ();
      for (unsigned int i = 0; i < glyphs.length; i++)
      {
        unsigned int fd = acc.fdSelect->get_fd (glyphs[i]);
        if (unlikely (fd >= acc.fdCount))
          return false;
        subr_subset_param_t  param;
        param.init (&parsed_charstrings[i],
                    &parsed_global_subrs,  &parsed_local_subrs[fd],
                    closures.global_closure, closures.local_closures[fd],
                    drop_hints);
        collect_subr_refs_in_str (parsed_charstrings[i], param);
      }
    }

    remaps.create (closures);

    return true;
  }

  bool encode_charstrings (ACC &acc, const hb_vector_t<hb_codepoint_t> &glyphs, str_buff_vec_t &buffArray) const
  {
    if (unlikely (!buffArray.resize (glyphs.length)))
      return false;
    for (unsigned int i = 0; i < glyphs.length; i++)
    {
      unsigned int  fd = acc.fdSelect->get_fd (glyphs[i]);
      if (unlikely (fd >= acc.fdCount))
        return false;
      if (unlikely (!encode_str (parsed_charstrings[i], fd, buffArray[i])))
        return false;
    }
    return true;
  }

  bool encode_subrs (const parsed_cs_str_vec_t &subrs, const subr_remap_t& remap, unsigned int fd, str_buff_vec_t &buffArray) const
  {
    unsigned int  count = remap.get_count ();

    if (unlikely (!buffArray.resize (count)))
      return false;
    for (unsigned int old_num = 0; old_num < subrs.length; old_num++)
    {
      hb_codepoint_t new_num = remap[old_num];
      if (new_num != CFF_UNDEF_CODE)
      {
        if (unlikely (!encode_str (subrs[old_num], fd, buffArray[new_num])))
          return false;
      }
    }
    return true;
  }

  bool encode_globalsubrs (str_buff_vec_t &buffArray)
  {
    return encode_subrs (parsed_global_subrs, remaps.global_remap, 0, buffArray);
  }

  bool encode_localsubrs (unsigned int fd, str_buff_vec_t &buffArray) const
  {
    return encode_subrs (parsed_local_subrs[fd], remaps.local_remaps[fd], fd, buffArray);
  }

  protected:
  struct drop_hints_param_t
  {
    drop_hints_param_t ()
      : seen_moveto (false),
        ends_in_hint (false),
        vsindex_dropped (false) {}

    bool  seen_moveto;
    bool  ends_in_hint;
    bool  vsindex_dropped;
  };

  bool drop_hints_in_subr (parsed_cs_str_t &str, unsigned int pos,
                           parsed_cs_str_vec_t &subrs, unsigned int subr_num,
                           const subr_subset_param_t &param, drop_hints_param_t &drop)
  {
    drop.ends_in_hint = false;
    bool has_hint = drop_hints_in_str (subrs[subr_num], param, drop);

    /* if this subr ends with a stem hint (i.e., not a number a potential argument for moveto),
     * then this entire subroutine must be a hint. drop its call. */
    if (drop.ends_in_hint)
    {
      str.values[pos].set_drop ();
      /* if this subr call is at the end of the parent subr, propagate the flag
       * otherwise reset the flag */
      if (!str.at_end (pos))
        drop.ends_in_hint = false;
    }

    return has_hint;
  }

  /* returns true if it sees a hint op before the first moveto */
  bool drop_hints_in_str (parsed_cs_str_t &str, const subr_subset_param_t &param, drop_hints_param_t &drop)
  {
    bool  seen_hint = false;

    for (unsigned int pos = 0; pos < str.values.length; pos++)
    {
      bool  has_hint = false;
      switch (str.values[pos].op)
      {
        case OpCode_callsubr:
          has_hint = drop_hints_in_subr (str, pos,
                                        *param.parsed_local_subrs, str.values[pos].subr_num,
                                        param, drop);

          break;

        case OpCode_callgsubr:
          has_hint = drop_hints_in_subr (str, pos,
                                        *param.parsed_global_subrs, str.values[pos].subr_num,
                                        param, drop);
          break;

        case OpCode_rmoveto:
        case OpCode_hmoveto:
        case OpCode_vmoveto:
          drop.seen_moveto = true;
          break;

        case OpCode_hintmask:
        case OpCode_cntrmask:
          if (drop.seen_moveto)
          {
            str.values[pos].set_drop ();
            break;
          }
          HB_FALLTHROUGH;

        case OpCode_hstemhm:
        case OpCode_vstemhm:
        case OpCode_hstem:
        case OpCode_vstem:
          has_hint = true;
          str.values[pos].set_drop ();
          if (str.at_end (pos))
            drop.ends_in_hint = true;
          break;

        case OpCode_dotsection:
          str.values[pos].set_drop ();
          break;

        default:
          /* NONE */
          break;
      }
      if (has_hint)
      {
        for (int i = pos - 1; i >= 0; i--)
        {
          parsed_cs_op_t  &csop = str.values[(unsigned)i];
          if (csop.for_drop ())
            break;
          csop.set_drop ();
          if (csop.op == OpCode_vsindexcs)
            drop.vsindex_dropped = true;
        }
        seen_hint |= has_hint;
      }
    }

    return seen_hint;
  }

  void collect_subr_refs_in_subr (parsed_cs_str_t &str, unsigned int pos,
                                  unsigned int subr_num, parsed_cs_str_vec_t &subrs,
                                  hb_set_t *closure,
                                  const subr_subset_param_t &param)
  {
    hb_set_add (closure, subr_num);
    collect_subr_refs_in_str (subrs[subr_num], param);
  }

  void collect_subr_refs_in_str (parsed_cs_str_t &str, const subr_subset_param_t &param)
  {
    for (unsigned int pos = 0; pos < str.values.length; pos++)
    {
      if (!str.values[pos].for_drop ())
      {
        switch (str.values[pos].op)
        {
          case OpCode_callsubr:
            collect_subr_refs_in_subr (str, pos,
                                       str.values[pos].subr_num, *param.parsed_local_subrs,
                                       param.local_closure, param);
            break;

          case OpCode_callgsubr:
            collect_subr_refs_in_subr (str, pos,
                                       str.values[pos].subr_num, *param.parsed_global_subrs,
                                       param.global_closure, param);
            break;

          default: break;
        }
      }
    }
  }

  bool encode_str (const parsed_cs_str_t &str, const unsigned int fd, str_buff_t &buff) const
  {
    buff.init ();
    str_encoder_t  encoder (buff);
    encoder.reset ();
    /* if a prefix (CFF1 width or CFF2 vsindex) has been removed along with hints,
     * re-insert it at the beginning of charstreing */
    if (str.has_prefix () && str.is_hint_dropped ())
    {
      encoder.encode_num (str.prefix_num ());
      if (str.prefix_op () != OpCode_Invalid)
        encoder.encode_op (str.prefix_op ());
    }
    for (unsigned int i = 0; i < str.get_count(); i++)
    {
      const parsed_cs_op_t  &opstr = str.values[i];
      if (!opstr.for_drop () && !opstr.for_skip ())
      {
        switch (opstr.op)
        {
          case OpCode_callsubr:
            encoder.encode_int (remaps.local_remaps[fd].biased_num (opstr.subr_num));
            encoder.encode_op (OpCode_callsubr);
            break;

          case OpCode_callgsubr:
            encoder.encode_int (remaps.global_remap.biased_num (opstr.subr_num));
            encoder.encode_op (OpCode_callgsubr);
            break;

          default:
            encoder.copy_str (opstr.str);
            break;
        }
      }
    }
    return !encoder.is_error ();
  }

  protected:
  subr_closures_t             closures;

  parsed_cs_str_vec_t          parsed_charstrings;
  parsed_cs_str_vec_t          parsed_global_subrs;
  hb_vector_t<parsed_cs_str_vec_t>  parsed_local_subrs;

  subr_remap_ts         remaps;

  private:
  typedef typename SUBRS::count_type subr_count_type;
};

} /* namespace CFF */

HB_INTERNAL bool
hb_plan_subset_cff_fdselect (const hb_vector_t<hb_codepoint_t> &glyphs,
                            unsigned int fdCount,
                            const CFF::FDSelect &src, /* IN */
                            unsigned int &subset_fd_count /* OUT */,
                            unsigned int &subset_fdselect_size /* OUT */,
                            unsigned int &subset_fdselect_format /* OUT */,
                            hb_vector_t<CFF::code_pair_t> &fdselect_ranges /* OUT */,
                            CFF::remap_t &fdmap /* OUT */);

HB_INTERNAL bool
hb_serialize_cff_fdselect (hb_serialize_context_t *c,
                          unsigned int num_glyphs,
                          const CFF::FDSelect &src,
                          unsigned int fd_count,
                          unsigned int fdselect_format,
                          unsigned int size,
                          const hb_vector_t<CFF::code_pair_t> &fdselect_ranges);

#endif /* HB_SUBSET_CFF_COMMON_HH */
