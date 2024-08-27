/*
 * Copyright (c) 2003, 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * Copyright (c) 2020, 2022, Huawei Technologies Co., Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "assembler_riscv.inline.hpp"
#include "code/vtableStubs.hpp"
#include "interp_masm_riscv.hpp"
#include "memory/resourceArea.hpp"
#include "oops/compiledICHolder.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klassVtable.hpp"
#include "runtime/sharedRuntime.hpp"
#include "vmreg_riscv.inline.hpp"
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif

// machine-dependent part of VtableStubs: create VtableStub of correct size and
// initialize its code

#define __ masm->

#ifndef PRODUCT
extern "C" void bad_compiled_vtable_index(JavaThread* thread, oop receiver, int index);
#endif

VtableStub* VtableStubs::create_vtable_stub(int vtable_index) {
  const int aarch64_code_length = VtableStub::pd_code_size_limit(true);
  VtableStub* s = new(aarch64_code_length) VtableStub(true, vtable_index);
  // Can be NULL if there is no free space in the code cache.
  if (s == NULL) {
    return NULL;
  }

  ResourceMark rm;
  CodeBuffer cb(s->entry_point(), aarch64_code_length);
  MacroAssembler* masm = new MacroAssembler(&cb);
  assert_cond(masm != NULL);

#ifndef PRODUCT
  if (CountCompiledCalls) {
    __ la(t2, ExternalAddress((address) SharedRuntime::nof_megamorphic_calls_addr()));
    __ add_memory_int64(Address(t2), 1);
  }
#endif

  // get receiver (need to skip return address on top of stack)
  assert(VtableStub::receiver_location() == j_rarg0->as_VMReg(), "receiver expected in j_rarg0");

  // get receiver klass
  address npe_addr = __ pc();
  __ load_klass(t2, j_rarg0);

#ifndef PRODUCT
  if (DebugVtables) {
    Label L;
    // check offset vs vtable length
    // __ lwu(t0, Address(t2, Klass::vtable_length_offset()));
    __ lwu(t0, Address(t2, InstanceKlass::vtable_length_offset()));
    __ mvw(t1, vtable_index * vtableEntry::size());
    __ bgt(t0, t1, L);
    __ enter();
    __ mv(x12, vtable_index);

    __ call_VM(noreg,
                CAST_FROM_FN_PTR(address, bad_compiled_vtable_index), j_rarg0, x12);
    __ leave();
    __ bind(L);
  }
#endif // PRODUCT

  __ lookup_virtual_method(t2, vtable_index, xmethod);

#ifndef PRODUCT
  if (DebugVtables) {
    Label L;
    __ beqz(xmethod, L);
    __ ld(t0, Address(xmethod, Method::from_compiled_offset()));
    __ bnez(t0, L);
    __ stop("Vtable entry is NULL");
    __ bind(L);
  }
#endif // PRODUCT

  // x10: receiver klass
  // xmethod: Method*
  // x12: receiver
  address ame_addr = __ pc();
  __ ld(t0, Address(xmethod, Method::from_compiled_offset()));
  __ jr(t0);

  __ flush();

  if (PrintMiscellaneous && (WizardMode || Verbose)) {
    tty->print_cr("vtable #%d at "PTR_FORMAT"[%d] left over: %d",
                  vtable_index, p2i(s->entry_point()),
                  (int)(s->code_end() - s->entry_point()),
                  (int)(s->code_end() - __ pc()));
  }
  guarantee(__ pc() <= s->code_end(), "overflowed buffer");

  s->set_exception_points(npe_addr, ame_addr);
  return s;
}


VtableStub* VtableStubs::create_itable_stub(int itable_index) {
  // Note well: pd_code_size_limit is the absolute minimum we can get
  // away with.  If you add code here, bump the code stub size
  // returned by pd_code_size_limit!
  const int code_length = VtableStub::pd_code_size_limit(false);
  VtableStub* s = new(code_length) VtableStub(false, itable_index);
  // Can be NULL if there is no free space in the code cache.
  if (s == NULL) {
    return NULL;
  }

  ResourceMark rm;
  CodeBuffer cb(s->entry_point(), code_length);
  MacroAssembler* masm = new MacroAssembler(&cb);
  assert_cond(masm != NULL);

#ifndef PRODUCT
  if (CountCompiledCalls) {
    __ la(x18, ExternalAddress((address) SharedRuntime::nof_megamorphic_calls_addr()));
    __ add_memory_int64(Address(x18), 1);
  }
#endif

  // Entry arguments:
  //  t2: CompiledICHolder
  //  j_rarg0: Receiver

  // This stub is called from compiled code which has no callee-saved registers,
  // so all registers except arguments are free at this point.
  const Register recv_klass_reg     = x18;
  const Register holder_klass_reg   = x19; // declaring interface klass (DECC)
  const Register resolved_klass_reg = xmethod; // resolved interface klass (REFC)
  const Register temp_reg           = x28;
  const Register temp_reg2          = x29;
  const Register icholder_reg       = t1;

  __ ld(resolved_klass_reg, Address(icholder_reg, CompiledICHolder::holder_klass_offset()));
  __ ld(holder_klass_reg,   Address(icholder_reg, CompiledICHolder::holder_metadata_offset()));

  Label L_no_such_interface;

  // get receiver klass (also an implicit null-check)
  assert(VtableStub::receiver_location() == j_rarg0->as_VMReg(), "receiver expected in j_rarg0");
  address npe_addr = __ pc();
  __ load_klass(recv_klass_reg, j_rarg0);

  // Receiver subtype check against REFC.
  // Destroys recv_klass_reg value.
  __ lookup_interface_method(// inputs: rec. class, interface
                             recv_klass_reg, resolved_klass_reg, noreg,
                             // outputs:  scan temp. reg1, scan temp. reg2
                             recv_klass_reg, temp_reg,
                             L_no_such_interface,
                             /*return_method=*/false);

  // Get selected method from declaring class and itable index
  __ load_klass(recv_klass_reg, j_rarg0);   // restore recv_klass_reg
  __ lookup_interface_method(// inputs: rec. class, interface, itable index
                             recv_klass_reg, holder_klass_reg, itable_index,
                             // outputs: method, scan temp. reg
                             xmethod, temp_reg,
                             L_no_such_interface);

  // method (xmethod): Method*
  // j_rarg0: receiver

#ifndef PRODUCT
  if (DebugVtables) {
    Label L2;
    __ beqz(xmethod, L2);
    __ ld(t0, Address(xmethod, Method::from_compiled_offset()));
    __ bnez(t0, L2);
    __ stop("compiler entrypoint is null");
    __ bind(L2);
  }
#endif // PRODUCT

  // xmethod: Method*
  // j_rarg0: receiver
  address ame_addr = __ pc();
  __ ld(t0, Address(xmethod, Method::from_compiled_offset()));
  __ jr(t0);

  __ bind(L_no_such_interface);
  __ far_jump(RuntimeAddress(StubRoutines::throw_IncompatibleClassChangeError_entry()));

  __ flush();

  if (PrintMiscellaneous && (WizardMode || Verbose)) {
    tty->print_cr("itable #%d at "PTR_FORMAT"[%d] left over: %d",
                  itable_index, p2i(s->entry_point()),
                  (int)(s->code_end() - s->entry_point()),
                  (int)(s->code_end() - __ pc()));
  }
  guarantee(__ pc() <= s->code_end(), "overflowed buffer");

  s->set_exception_points(npe_addr, ame_addr);
  return s;
}


int VtableStub::pd_code_size_limit(bool is_vtable_stub) {
  if (TraceJumps || DebugVtables || CountCompiledCalls || VerifyOops) {
    return 1000;
  }
  int size = is_vtable_stub ? 60 : 192; // Plain + safety
  return size;

}

int VtableStub::pd_code_alignment() { return 4; }
