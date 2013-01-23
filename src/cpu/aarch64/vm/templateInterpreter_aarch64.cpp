/*
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/assembler.hpp"
#include "interpreter/bytecodeHistogram.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterGenerator.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/templateTable.hpp"
#include "interpreter/bytecodeTracer.hpp"
#include "oops/arrayOop.hpp"
#include "oops/methodDataOop.hpp"
#include "oops/methodOop.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiThreadState.hpp"
#include "runtime/arguments.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/timer.hpp"
#include "runtime/vframeArray.hpp"
#include "utilities/debug.hpp"
#include <sys/types.h>

#ifndef PRODUCT
#include "oops/methodOop.hpp"
#endif // !PRODUCT

#include "../../../../../../simulator/simulator.hpp"

#define __ _masm->

#ifndef CC_INTERP

//-----------------------------------------------------------------------------

extern "C" void entry(CodeBuffer*);

//-----------------------------------------------------------------------------

address TemplateInterpreterGenerator::generate_StackOverflowError_handler() {
  address entry = __ pc();

#ifdef ASSERT
  {
    Label L;
    __ ldr(rscratch1, Address(rfp,
		       frame::interpreter_frame_monitor_block_top_offset *
		       wordSize));
    __ mov(rscratch2, sp);
    __ cmp(rscratch1, rscratch2); // maximal rsp for current rfp (stack
                           // grows negative)
    __ br(Assembler::HS, L); // check if frame is complete
    __ stop ("interpreter frame not set up");
    __ bind(L);
  }
#endif // ASSERT
  // Restore bcp under the assumption that the current frame is still
  // interpreted
  __ restore_bcp();

  // expression stack must be empty before entering the VM if an
  // exception happened
  __ empty_expression_stack();
  // throw exception
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::throw_StackOverflowError));
  return entry;
}

address TemplateInterpreterGenerator::generate_ArrayIndexOutOfBounds_handler(
        const char* name) {
  address entry = __ pc();
  // expression stack must be empty before entering the VM if an
  // exception happened
  __ empty_expression_stack();
  // setup parameters
  // ??? convention: expect aberrant index in register r1
  __ movw(c_rarg2, r1);
  __ mov(c_rarg1, (address)name);
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::
                              throw_ArrayIndexOutOfBoundsException),
             c_rarg1, c_rarg2);
  return entry;
}

address TemplateInterpreterGenerator::generate_ClassCastException_handler() {
  address entry = __ pc();

  // object is at TOS
  __ pop(c_rarg1);

  // expression stack must be empty before entering the VM if an
  // exception happened
  __ empty_expression_stack();

  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::
                              throw_ClassCastException),
             c_rarg1);
  return entry;
}

address TemplateInterpreterGenerator::generate_exception_handler_common(
        const char* name, const char* message, bool pass_oop) {
  assert(!pass_oop || message == NULL, "either oop or message but not both");
  address entry = __ pc();
  if (pass_oop) {
    // object is at TOS
    __ pop(c_rarg2);
  }
  // expression stack must be empty before entering the VM if an
  // exception happened
  __ empty_expression_stack();
  // setup parameters
  __ lea(c_rarg1, Address((address)name));
  if (pass_oop) {
    __ call_VM(r0, CAST_FROM_FN_PTR(address,
				    InterpreterRuntime::
				    create_klass_exception),
               c_rarg1, c_rarg2);
  } else {
    // kind of lame ExternalAddress can't take NULL because
    // external_word_Relocation will assert.
    if (message != NULL) {
      __ lea(c_rarg2, Address((address)message));
    } else {
      __ mov(c_rarg2, NULL_WORD);
    }
    __ call_VM(r0,
               CAST_FROM_FN_PTR(address, InterpreterRuntime::create_exception),
               c_rarg1, c_rarg2);
  }
  // throw exception
  __ b(address(Interpreter::throw_exception_entry()));
  return entry;
}

address TemplateInterpreterGenerator::generate_continuation_for(TosState state) { __ call_Unimplemented(); return 0; }


address TemplateInterpreterGenerator::generate_return_entry_for(TosState state, int step) {
  address entry = __ pc();

  // Restore stack bottom in case i2c adjusted stack
  __ ldr(esp, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  // and NULL it as marker that esp is now tos until next java call
  __ str(zr, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  __ restore_bcp();
  __ restore_locals();
  __ restore_constant_pool_cache();
  __ get_method(rmethod);

  Label L_got_cache, L_giant_index;
  if (EnableInvokeDynamic) {
    __ ldrb(r1, Address(rbcp, 0));
    __ cmpw(r1, Bytecodes::_invokedynamic);
    __ br(Assembler::EQ, L_giant_index);
  }
  // Pop N words from the stack
  __ get_cache_and_index_at_bcp(r1, r2, 1, sizeof(u2));
  __ bind(L_got_cache);
  __ ldrb(r1, Address(r1,
		     in_bytes(constantPoolCacheOopDesc::base_offset()) +
		     3 * wordSize));
  __ add(esp, esp, r1, Assembler::LSL, 3);
#ifdef ASSERT
  __ spillcheck(rscratch1, rscratch2);
#endif // ASSERT
#ifndef PRODUCT
  // tell the simulator that the method has been reentered
  if (NotifySimulator) {
    __ notify(Assembler::method_reentry);
  }
#endif
  __ dispatch_next(state, step);

  // out of the main line of code...
  if (EnableInvokeDynamic) {
    __ bind(L_giant_index);
    __ get_cache_and_index_at_bcp(r1, r2, 1, sizeof(u4));
    __ b(L_got_cache);
  }

  return entry;
}

address TemplateInterpreterGenerator::generate_deopt_entry_for(TosState state,
                                                               int step) { __ call_Unimplemented(); return 0; }

int AbstractInterpreter::BasicType_as_index(BasicType type) {
  int i = 0;
  switch (type) {
    case T_BOOLEAN: i = 0; break;
    case T_CHAR   : i = 1; break;
    case T_BYTE   : i = 2; break;
    case T_SHORT  : i = 3; break;
    case T_INT    : i = 4; break;
    case T_LONG   : i = 5; break;
    case T_VOID   : i = 6; break;
    case T_FLOAT  : i = 7; break;
    case T_DOUBLE : i = 8; break;
    case T_OBJECT : i = 9; break;
    case T_ARRAY  : i = 9; break;
    default       : ShouldNotReachHere();
  }
  assert(0 <= i && i < AbstractInterpreter::number_of_result_handlers,
         "index out of bounds");
  return i;
}


address TemplateInterpreterGenerator::generate_result_handler_for(
        BasicType type) {
    address entry = __ pc();
  switch (type) {
  case T_BOOLEAN: __ uxtb(r0, r0);        break;
  case T_CHAR   : __ uxth(r0, r0);       break;
  case T_BYTE   : __ sxtb(r0, r0);        break;
  case T_SHORT  : __ sxth(r0, r0);        break;
  case T_INT    : __ uxtw(r0, r0);        break;  // FIXME: We almost certainly don't need this
  case T_LONG   : /* nothing to do */        break;
  case T_VOID   : /* nothing to do */        break;
  case T_FLOAT  : /* nothing to do */        break;
  case T_DOUBLE : /* nothing to do */        break;
  case T_OBJECT :
    // retrieve result from frame
    __ ldr(r0, Address(rfp, frame::interpreter_frame_oop_temp_offset*wordSize));
    // and verify it
    __ verify_oop(r0);
    break;
  default       : ShouldNotReachHere();
  }
  __ ret(lr);                                  // return from result handler
  return entry;
}

address TemplateInterpreterGenerator::generate_safept_entry_for(
        TosState state,
        address runtime_entry) {
  address entry = __ pc();
  __ push(state);
  __ call_VM(noreg, runtime_entry);
  __ dispatch_via(vtos, Interpreter::_normal_table.table_for(vtos));
  return entry;
}

// Helpers for commoning out cases in the various type of method entries.
//


// increment invocation count & check for overflow
//
// Note: checking for negative value instead of overflow
//       so we have a 'sticky' overflow test
//
// rbx: method
// ecx: invocation counter
//
void InterpreterGenerator::generate_counter_incr(
        Label* overflow,
        Label* profile_method,
        Label* profile_method_continue) {
  // FIXME: We'll need this once we have a compiler.
}

void InterpreterGenerator::generate_counter_overflow(Label* do_continue) {
  // FIXME: We'll need this once we have a compiler.
}

// See if we've got enough room on the stack for locals plus overhead.
// The expression stack grows down incrementally, so the normal guard
// page mechanism will work for that.
//
// NOTE: Since the additional locals are also always pushed (wasn't
// obvious in generate_method_entry) so the guard should work for them
// too.
//
// Args:
<<<<<<< HEAD
//      rdx: number of additional locals this frame needs (what we must check)
//      rbx: methodOop
=======
//      r3: number of additional locals this frame needs (what we must check)
//      rmethod: Method*
>>>>>>> 1d142b0... Stub for throw_exception, branch relocs, generate stack_overflow_check.
//
// Kills:
//      r0
void InterpreterGenerator::generate_stack_overflow_check(void) {

  // monitor entry size: see picture of stack set
  // (generate_method_entry) and frame_amd64.hpp
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

  // total overhead size: entry_size + (saved rbp through expr stack
  // bottom).  be sure to change this if you add/subtract anything
  // to/from the overhead area
  const int overhead_size =
    -(frame::interpreter_frame_initial_sp_offset * wordSize) + entry_size;

  const int page_size = os::vm_page_size();

  Label after_frame_check;

  // see if the frame is greater than one page in size. If so,
  // then we need to verify there is enough stack space remaining
  // for the additional locals.
  __ cmp(r3, (page_size - overhead_size) / Interpreter::stackElementSize);
  __ br(Assembler::LS, after_frame_check);

  // compute rsp as if this were going to be the last frame on
  // the stack before the red zone

  const Address stack_base(rthread, Thread::stack_base_offset());
  const Address stack_size(rthread, Thread::stack_size_offset());

  // locals + overhead, in bytes
  __ mov(r0, overhead_size);
  __ add(r0, r0, r3, Assembler::LSL, Interpreter::logStackElementSize);  // 2 slots per parameter.

  __ ldr(rscratch1, stack_base);
  __ ldr(rscratch2, stack_size);

#ifdef ASSERT
  Label stack_base_okay, stack_size_okay;
  // verify that thread stack base is non-zero
  __ cbnz(rscratch1, stack_base_okay);
  __ stop("stack base is zero");
  __ bind(stack_base_okay);
  // verify that thread stack size is non-zero
  __ cbnz(rscratch2, stack_size_okay);
  __ stop("stack size is zero");
  __ bind(stack_size_okay);
#endif

  // Add stack base to locals and subtract stack size
  __ sub(rscratch1, rscratch1, rscratch2); // Stack limit
  __ add(r0, r0, rscratch1);

  // Use the maximum number of pages we might bang.
  const int max_pages = StackShadowPages > (StackRedPages+StackYellowPages) ? StackShadowPages :
                                                                              (StackRedPages+StackYellowPages);

  // add in the red and yellow zone sizes
  __ add(r0, r0, max_pages * page_size * 2);

  // check against the current stack bottom
  __ cmp(sp, r0);
  __ br(Assembler::LO, after_frame_check);

  // Note: the restored frame is not necessarily interpreted.
  // Use the shared runtime version of the StackOverflowError.
  assert(StubRoutines::throw_StackOverflowError_entry() != NULL, "stub not yet generated");
  __ b(RuntimeAddress(StubRoutines::throw_StackOverflowError_entry()));

  // all done with frame size check
  __ bind(after_frame_check);
}

// Allocate monitor and lock method (asm interpreter)
//
// Args:
//      rmethod: methodOop
//      rlocals: locals
//
// Kills:
//      r0
//      c_rarg0, c_rarg1, c_rarg2, c_rarg3, ...(param regs)
//      rscratch1, rscratch2 (scratch regs)
void InterpreterGenerator::lock_method(void) {
  // synchronize method
  const Address access_flags(rmethod, methodOopDesc::access_flags_offset());
  const Address monitor_block_top(
        rfp,
        frame::interpreter_frame_monitor_block_top_offset * wordSize);
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

#ifdef ASSERT
  {
    Label L;
    __ ldrw(r0, access_flags);
    __ tst(r0, JVM_ACC_SYNCHRONIZED);
    __ br(Assembler::NE, L);
    __ stop("method doesn't need synchronization");
    __ bind(L);
  }
#endif // ASSERT

  // get synchronization object
  {
    const int mirror_offset = in_bytes(Klass::java_mirror_offset());
    Label done;
    __ ldrw(r0, access_flags);
    __ tst(r0, JVM_ACC_STATIC);
    // get receiver (assume this is frequent case)
    __ ldr(r0, Address(rlocals, Interpreter::local_offset_in_bytes(0)));
    __ br(Assembler::EQ, done);
    __ ldr(r0, Address(rmethod, methodOopDesc::constants_offset()));
    __ ldr(r0, Address(r0,
                           constantPoolOopDesc::pool_holder_offset_in_bytes()));
    __ ldr(r0, Address(r0, mirror_offset));

#ifdef ASSERT
    {
      Label L;
      __ cbnz(r0, L);
      __ stop("synchronization object is NULL");
      __ bind(L);
    }
#endif // ASSERT

    __ bind(done);
  }

  // add space for monitor & lock
  __ sub(sp, sp, entry_size); // add space for a monitor entry
  __ sub(esp, esp, entry_size);
  __ mov(rscratch1, esp);
  __ str(rscratch1, monitor_block_top);  // set new monitor block top
  // store object
  __ str(r0, Address(esp, BasicObjectLock::obj_offset_in_bytes()));
  __ mov(c_rarg1, esp); // object address
  __ lock_object(c_rarg1);
}

// Generate a fixed interpreter frame. This is identical setup for
// interpreted methods and for native methods hence the shared code.
//
// Args:
//      lr: return address
//      rmethod: Method*
//      rlocals: pointer to locals
//      rcpool: cp cache
//      stack_pointer: previous sp
void TemplateInterpreterGenerator::generate_fixed_frame(bool native_call, Register stack_pointer) {
  // initialize fixed part of activation frame
  // return address does not need pushing as it was never popped
  // from the stack. instead enter() will save lr and leave will
  // restore it later

  // Save previous sp
  if (stack_pointer == sp) {
    __ mov(rscratch1, sp);
    stack_pointer = rscratch1;
  }

  // Save previous sp.  If this is a native call, the higher word of
  // this pair will be used for oop_temp so it must be zeroed.
  // FIXME: Should we not always push zr?
  if (native_call)
    __ stp(stack_pointer, zr, Address(__ pre(sp, -2 * wordSize)));
  else
    __ str(stack_pointer, Address(__ pre(sp, -2 * wordSize)));

  __ enter();          // save old & set new rfp

  // set sender sp
  // leave last_sp as null
  __ stp(zr, esp, Address(__ pre(sp, -2 * wordSize)));
  __ ldr(rscratch1, Address(rmethod, Method::const_offset()));      // get ConstMethod
  __ add(rbcp, rscratch1, in_bytes(ConstMethod::codes_offset())); // get codebase
  if (ProfileInterpreter) {
    // Label method_data_continue;
    // __ movptr(rdx, Address(rbx, in_bytes(methodOopDesc::method_data_offset())));
    // __ testptr(rdx, rdx);
    // __ jcc(Assembler::zero, method_data_continue);
    // __ addptr(rdx, in_bytes(methodDataOopDesc::data_offset()));
    // __ bind(method_data_continue);
    // __ push(rdx);      // set the mdp (method data pointer)
  } else {
  }
  __ stp(zr, rmethod, Address(__ pre(sp, -2 * wordSize)));        // save Method*

  __ ldr(rcpool, Address(rmethod, Method::const_offset()));
  __ ldr(rcpool, Address(rcpool, ConstMethod::constants_offset()));
  __ ldr(rcpool, Address(rcpool, ConstantPool::cache_offset_in_bytes()));
  __ stp(rlocals, rcpool, Address(__ pre(sp, -2 * wordSize)));

  if (native_call) {
    __ stp(zr, zr, Address(__ pre(sp, -2 * wordSize))); // no bcp
  } else {
    __ stp(zr, rbcp, Address(__ pre(sp, -2 * wordSize))); // set bcp
  }

  __ mov(esp, sp); // Initialize expression stack pointer

  // Move SP out of the way
  if (! native_call) {
    __ ldrh(rscratch1, Address(rmethod, Method::max_stack_offset()));
    __ add(rscratch1, rscratch1, frame::interpreter_frame_monitor_size()
	   + (EnableInvokeDynamic ? 2 : 0));
    __ sub(rscratch1, sp, rscratch1, ext::uxtw, 3);
    __ andr(sp, rscratch1, -16);
  }

  __ str(esp, Address(esp)); // Initial ESP
}

// End of helpers

// Various method entries
//------------------------------------------------------------------------------------------------------------------------
//
//

// Call an accessor method (assuming it is resolved, otherwise drop
// into vanilla (slow path) entry
address InterpreterGenerator::generate_accessor_entry(void) {
  return NULL;
}

// Method entry for java.lang.ref.Reference.get.
address InterpreterGenerator::generate_Reference_get_entry(void) {
  return NULL;
}

void InterpreterGenerator::bang_stack_shadow_pages(bool native_call) {
  // Bang each page in the shadow zone. We can't assume it's been done for
  // an interpreter frame with greater than a page of locals, so each page
  // needs to be checked.  Only true for non-native.
  if (UseStackBanging) {
    const int start_page = native_call ? StackShadowPages : 1;
    const int page_size = os::vm_page_size();
    for (int pages = start_page; pages <= StackShadowPages ; pages++) {
      __ sub(rscratch2, sp, pages*page_size);
      __ ldr(zr, Address(rscratch2));
    }
  }
}


// Interpreter stub for calling a native method. (asm interpreter)
// This sets up a somewhat different looking stack for calling the
// native method than the typical interpreter frame setup.
address InterpreterGenerator::generate_native_entry(bool synchronized) {
  // determine code generation flags
  bool inc_counter  = UseCompiler || CountCompiledCalls;

  // rmethod: methodOop
  // rscratch1: sender sp

  address entry_point = __ pc();

  const Address size_of_parameters(rmethod, methodOopDesc::
                                        size_of_parameters_offset());
  const Address invocation_counter(rmethod, methodOopDesc::
                                        invocation_counter_offset() +
                                        InvocationCounter::counter_offset());
  const Address access_flags      (rmethod, methodOopDesc::access_flags_offset());

  // get parameter size (always needed)
  __ load_unsigned_short(r2, size_of_parameters);

  // native calls don't need the stack size check since they have no
  // expression stack and the arguments are already on the stack and
  // we only add a handful of words to the stack

  // rmethod: methodOop
  // r2: size of parameters
  // rscratch1: sender sp

  // for natives the size of locals is zero

  // compute beginning of parameters (rlocals)
  __ add(rlocals, esp, r2, ext::uxtx, 3);
  __ add(rlocals, rlocals, -wordSize);

  // save sp
  __ mov(rscratch1, sp);
  __ andr(sp, esp, -16);

  if (inc_counter) {
    __ prfm(invocation_counter);  // (pre-)fetch invocation count
  }

  // initialize fixed part of activation frame
  generate_fixed_frame(true, rscratch1);
#ifndef PRODUCT
  // tell the simulator that a method has been entered
  if (NotifySimulator) {
    __ notify(Assembler::method_entry);
  }
#endif

  // make sure method is native & not abstract
#ifdef ASSERT
  __ ldrw(r0, access_flags);
  {
    Label L;
    __ tst(r0, JVM_ACC_NATIVE);
    __ br(Assembler::NE, L);
    __ stop("tried to execute non-native method as native");
    __ bind(L);
  }
  {
    Label L;
    __ tst(r0, JVM_ACC_ABSTRACT);
    __ br(Assembler::EQ, L);
    __ stop("tried to execute abstract method in interpreter");
    __ bind(L);
  }
#endif

  // Since at this point in the method invocation the exception
  // handler would try to exit the monitor of synchronized methods
  // which hasn't been entered yet, we set the thread local variable
  // _do_not_unlock_if_synchronized to true. The remove_activation
  // will check this flag.

   const Address do_not_unlock_if_synchronized(rthread,
        in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
  __ mov(rscratch2, true);
  __ strb(rscratch2, do_not_unlock_if_synchronized);

  // increment invocation count & check for overflow
  Label invocation_counter_overflow;
  if (inc_counter) {
    generate_counter_incr(&invocation_counter_overflow, NULL, NULL);
  }

  Label continue_after_compile;
  __ bind(continue_after_compile);

  bang_stack_shadow_pages(true);

  // reset the _do_not_unlock_if_synchronized flag
  __ strb(zr, do_not_unlock_if_synchronized);

  // check for synchronized methods
  // Must happen AFTER invocation_counter check and stack overflow check,
  // so method is not locked if overflows.
  if (synchronized) {
    lock_method();
  } else {
    // no synchronization necessary
#ifdef ASSERT
    {
      Label L;
      __ ldrw(r0, access_flags);
      __ tst(r0, JVM_ACC_SYNCHRONIZED);
      __ br(Assembler::EQ, L);
      __ stop("method needs synchronization");
      __ bind(L);
    }
#endif
  }

  // start execution
#ifdef ASSERT
  {
    Label L;
    const Address monitor_block_top(rfp,
                 frame::interpreter_frame_monitor_block_top_offset * wordSize);
    __ ldr(rscratch1, monitor_block_top);
    __ cmp(esp, rscratch1);
    __ br(Assembler::EQ, L);
    __ stop("broken stack frame setup in interpreter");
    __ bind(L);
  }
#endif

  // jvmti support
  __ notify_method_entry();

  // work registers
  const Register t = r17;
  const Register result_handler = r19;

  // allocate space for parameters
  __ load_unsigned_short(t,
                         Address(rmethod,
                                 Method::size_of_parameters_offset()));

  __ sub(rscratch1, esp, t, ext::uxtx, Interpreter::logStackElementSize);
  __ andr(sp, rscratch1, -16);
  __ mov(esp, rscratch1);

  // get signature handler
  {
    Label L;
    __ ldr(t, Address(rmethod, methodOopDesc::signature_handler_offset()));
    __ cbz(t, L);
    __ call_VM(noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::prepare_native_call),
               rmethod);
    __ ldr(t, Address(rmethod, methodOopDesc::signature_handler_offset()));
    __ bind(L);
  }

  // call signature handler
  assert(InterpreterRuntime::SignatureHandlerGenerator::from() == rlocals,
         "adjust this code");
  assert(InterpreterRuntime::SignatureHandlerGenerator::to() == sp,
         "adjust this code");
  assert(InterpreterRuntime::SignatureHandlerGenerator::temp() == rscratch1,
          "adjust this code");

<<<<<<< HEAD
  // The generated handlers do not touch RBX (the method oop).
=======
  // The generated handlers do not touch rmethod (the method).
>>>>>>> 4d5df24... First cut of method handles.
  // However, large signatures cannot be cached and are generated
  // each time here.  The slow-path generator can do a GC on return,
  // so we must reload it after the call.
  __ blr(t);
  __ get_method(rmethod);        // slow path can do a GC, reload rmethod


  // result handler is in r0
  // set result handler
<<<<<<< HEAD
  __ ldr(r0, Address(rfp,
		     (frame::interpreter_frame_result_handler_offset) * wordSize));
=======
  __ mov(result_handler, r0);
>>>>>>> c7f03c5... Get rid of result handler slot in stack frame
  // pass mirror handle if static call
  {
    Label L;
    const int mirror_offset = in_bytes(Klass::java_mirror_offset());
    __ ldrw(t, Address(rmethod, methodOopDesc::access_flags_offset()));
    __ tst(t, JVM_ACC_STATIC);
    __ br(Assembler::EQ, L);
    // get mirror
    __ ldr(t, Address(rmethod, methodOopDesc::constants_offset()));
    __ ldr(t, Address(t, constantPoolOopDesc::pool_holder_offset_in_bytes()));
    __ ldr(t, Address(t, mirror_offset));
    // copy mirror into activation frame
    __ str(t, Address(rfp, frame::interpreter_frame_oop_temp_offset * wordSize));
    // pass handle to mirror
    __ add(c_rarg1, rfp, frame::interpreter_frame_oop_temp_offset * wordSize);
    __ bind(L);
  }

  // get native function entry point
  {
    Label L;
    __ ldr(r0, Address(rmethod, methodOopDesc::native_function_offset()));
    address unsatisfied = (SharedRuntime::native_method_throw_unsatisfied_link_error_entry());
    __ mov(rscratch2, unsatisfied);
    __ ldr(rscratch2, rscratch2);
    __ cmp(r0, rscratch2);
    __ br(Assembler::NE, L);
    __ call_VM(noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::prepare_native_call),
               rmethod);
    __ get_method(rmethod);
<<<<<<< HEAD
    __ verify_oop(rmethod);
    __ ldr(r0, Address(rmethod, methodOopDesc::native_function_offset()));
=======
    __ ldr(r10, Address(rmethod, Method::native_function_offset()));
>>>>>>> 4d5df24... First cut of method handles.
    __ bind(L);
  }

  // pass JNIEnv
  __ add(c_rarg0, rthread, in_bytes(JavaThread::jni_environment_offset()));

  // It is enough that the pc() points into the right code
  // segment. It does not have to be the correct return pc.
  __ set_last_Java_frame(esp, rfp, (address) __ pc());

  // change thread state
#ifdef ASSERT
  {
    Label L;
    __ ldr(t, Address(rthread, JavaThread::thread_state_offset()));
    __ cmp(t, _thread_in_Java);
    __ br(Assembler::EQ, L);
    __ stop("Wrong thread state in native stub");
    __ bind(L);
  }
#endif

  // Change state to native
  __ mov(rscratch1, _thread_in_native);
  __ str(rscratch1, Address(rthread, JavaThread::thread_state_offset()));

  // load call format
  __ ldrw(rscratch1, Address(rmethod, methodOopDesc::call_format_offset()));

  // Call the native method.
  __ brx86(r10, rscratch1);

  // make room for the pushes we're about to do
  __ sub(rscratch1, esp, 4 * wordSize);
  __ andr(sp, rscratch1, -16);

  // NOTE: The order of these pushes is known to frame::interpreter_frame_result
  // in order to extract the result of a method call. If the order of these
  // pushes change or anything else is added to the stack then the code in
  // interpreter_frame_result must also change.
  __ push(dtos);
  __ push(ltos);

  // change thread state
  __ mov(rscratch1, _thread_in_native_trans);
  __ str(rscratch1, Address(rthread, JavaThread::thread_state_offset()));

  if (os::is_MP()) {
    if (UseMembar) {
      // Force this write out before the read below
      __ dmb(Assembler::SY);
    } else {
      // Write serialization page so VM thread can do a pseudo remote membar.
      // We use the current thread pointer to calculate a thread specific
      // offset to write to within the page. This minimizes bus traffic
      // due to cache line collision.
      __ serialize_memory(rthread, rscratch2);
    }
  }

  // check for safepoint operation in progress and/or pending suspend requests
  {
    Label Continue;
    __ mov(rscratch2, SafepointSynchronize::address_of_state());
    __ ldr(rscratch2, rscratch2);
    assert(SafepointSynchronize::_not_synchronized == 0,
	   "SafepointSynchronize::_not_synchronized");
    Label L;
    __ cbnz(rscratch2, L);
    __ ldrw(rscratch2, Address(rthread, JavaThread::suspend_flags_offset()));
    __ cbz(rscratch2, Continue);
    __ bind(L);

    // Don't use call_VM as it will see a possible pending exception
    // and forward it and never return here preventing us from
    // clearing _last_native_pc down below.  Also can't use
    // call_VM_leaf either as it will check to see if r13 & r14 are
    // preserved and correspond to the bcp/locals pointers. So we do a
    // runtime call by hand.
    //
    __ mov(c_rarg0, rthread);
    __ mov(rscratch2, CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans));
    __ brx86(rscratch2, 1, 0, 0);
    __ reinit_heapbase();
    __ bind(Continue);
  }

  // change thread state
  __ mov(rscratch1, _thread_in_Java);
  __ str(rscratch1, Address(rthread, JavaThread::thread_state_offset()));

  // reset_last_Java_frame
  __ reset_last_Java_frame(true, true);

  // reset handle block
  __ ldr(t, Address(rthread, JavaThread::active_handles_offset()));
  __ str(zr, Address(t, JNIHandleBlock::top_offset_in_bytes()));

  // If result is an oop unbox and store it in frame where gc will see it
  // and result handler will pick it up

  {
    Label no_oop, store_result;
    __ adr(t, ExternalAddress(AbstractInterpreter::result_handler(T_OBJECT)));
    __ cmp(t, result_handler);
    __ br(Assembler::NE, no_oop);
    // retrieve result
    __ pop(ltos);
    __ cbz(r0, store_result);
    __ ldr(r0, Address(r0, 0));
    __ bind(store_result);
    __ str(r0, Address(rfp, frame::interpreter_frame_oop_temp_offset*wordSize));
    // keep stack depth as expected by pushing oop which will eventually be discarded
    __ push(ltos);
    __ bind(no_oop);
  }


  {
    Label no_reguard;
    __ lea(rscratch1, Address(rthread, in_bytes(JavaThread::stack_guard_state_offset())));
    __ ldr(rscratch1, Address(rscratch1));
    __ cmp(rscratch1, JavaThread::stack_guard_yellow_disabled);
    __ br(Assembler::NE, no_reguard);

    __ pusha(); // XXX only save smashed registers
    __ mov(c_rarg0, rthread);
    __ mov(rscratch2, CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages));
    __ brx86(rscratch2, 0, 0, 0);
    __ popa(); // XXX only restore smashed registers
    __ reinit_heapbase();
    __ bind(no_reguard);
  }


  // The method register is junk from after the thread_in_native transition
  // until here.  Also can't call_VM until the bcp has been
  // restored.  Need bcp for throwing exception below so get it now.
  __ get_method(rmethod);

  // restore bcp to have legal interpreter frame, i.e., bci == 0 <=>
  // rbcp == code_base()
  __ ldr(rbcp, Address(rmethod, methodOopDesc::const_offset()));   // get constMethodOop
  __ add(rbcp, rbcp, in_bytes(constMethodOopDesc::codes_offset()));          // get codebase
  // handle exceptions (exception handling will handle unlocking!)
  {
    Label L;
    __ ldr(rscratch1, Address(rthread, Thread::pending_exception_offset()));
    __ cbz(rscratch1, L);
    // Note: At some point we may want to unify this with the code
    // used in call_VM_base(); i.e., we should use the
    // StubRoutines::forward_exception code. For now this doesn't work
    // here because the rsp is not correctly set at this point.
    __ MacroAssembler::call_VM(noreg,
                               CAST_FROM_FN_PTR(address,
                               InterpreterRuntime::throw_pending_exception));
    __ should_not_reach_here();
    __ bind(L);
  }

  // do unlocking if necessary
  {
    Label L;
    __ ldrw(t, Address(rmethod, methodOopDesc::access_flags_offset()));
    __ tst(t, JVM_ACC_SYNCHRONIZED);
    __ br(Assembler::EQ, L);
    // the code below should be shared with interpreter macro
    // assembler implementation
    {
      Label unlock;
      // BasicObjectLock will be first in list, since this is a
      // synchronized method. However, need to check that the object
      // has not been unlocked by an explicit monitorexit bytecode.

      // monitor expect in c_rarg1 for slow unlock path
      __ lea (c_rarg1, Address(rfp,   // address of first monitor
			       (intptr_t)(frame::interpreter_frame_initial_sp_offset *
					  wordSize - sizeof(BasicObjectLock))));

      __ ldr(t, Address(c_rarg1, BasicObjectLock::obj_offset_in_bytes()));
      __ cbnz(t, unlock);

      // Entry already unlocked, need to throw exception
      __ MacroAssembler::call_VM(noreg,
                                 CAST_FROM_FN_PTR(address,
                   InterpreterRuntime::throw_illegal_monitor_state_exception));
      __ should_not_reach_here();

      __ bind(unlock);
      __ unlock_object(c_rarg1);
    }
    __ bind(L);
  }

  // jvmti support
  // Note: This must happen _after_ handling/throwing any exceptions since
  //       the exception handler code notifies the runtime of method exits
  //       too. If this happens before, method entry/exit notifications are
  //       not properly paired (was bug - gri 11/22/99).
  __ notify_method_exit(vtos, InterpreterMacroAssembler::NotifyJVMTI);

  // restore potential result in edx:eax, call result handler to
  // restore potential result in ST0 & handle result

  __ pop(ltos);
  __ pop(dtos);

<<<<<<< HEAD
  __ ldr(t, Address(rfp,
		    (frame::interpreter_frame_result_handler_offset) * wordSize));
  __ br(t);
=======
  __ blr(result_handler);
>>>>>>> c7f03c5... Get rid of result handler slot in stack frame

  // remove activation
  __ ldr(esp, Address(rfp,
		    frame::interpreter_frame_sender_sp_offset *
		    wordSize)); // get sender sp
  // remove frame anchor
  __ leave();
  // restore sp
  __ ldr(rscratch1, Address(__ post(sp, 2 * wordSize)));
  __ mov(sp, rscratch1);

  if (inc_counter) {
    // Handle overflow of counter and compile method
    __ bind(invocation_counter_overflow);
    generate_counter_overflow(&continue_after_compile);
  }

  __ ret(lr);

  return entry_point;
}

//
// Generic interpreted method entry to (asm) interpreter
//
address InterpreterGenerator::generate_normal_entry(bool synchronized) {
  // determine code generation flags
  bool inc_counter  = UseCompiler || CountCompiledCalls;

  // rscratch1: sender sp
  address entry_point = __ pc();

  const Address size_of_parameters(rmethod,
                                   methodOopDesc::size_of_parameters_offset());
  const Address size_of_locals(rmethod, methodOopDesc::size_of_locals_offset());
  const Address invocation_counter(rmethod,
                                   methodOopDesc::invocation_counter_offset() +
                                   InvocationCounter::counter_offset());
  const Address access_flags(rmethod, methodOopDesc::access_flags_offset());

  // get parameter size (always needed)
  __ load_unsigned_short(r2, size_of_parameters);

<<<<<<< HEAD
  // r1: methodOop
=======
>>>>>>> 1d142b0... Stub for throw_exception, branch relocs, generate stack_overflow_check.
  // r2: size of parameters

  __ load_unsigned_short(r3, size_of_locals); // get size of locals in words
  __ sub(r3, r3, r2); // r3 = no. of additional locals

  // see if we've got enough room on the stack for locals plus overhead.
  generate_stack_overflow_check();

  // compute beginning of parameters (rlocals)
  __ add(rlocals, esp, r2, ext::uxtx, 3);
  __ sub(rlocals, rlocals, wordSize);

  // Pass previous SP to save in generate_fixed_frame()
  __ mov(r10, sp);

  // Make room for locals
  __ sub(rscratch1, esp, r3, ext::uxtx, 3);
  __ andr(sp, rscratch1, -16);

  // r3 - # of additional locals
  // allocate space for locals
  // explicitly initialize locals
  {
    Label exit, loop;
    __ ands(zr, r3, r3);
    __ br(Assembler::LE, exit); // do nothing if r3 <= 0
    __ bind(loop);
    __ str(zr, Address(__ post(rscratch1, wordSize)));
    __ sub(r3, r3, 1); // until everything initialized
    __ cbnz(r3, loop);
    __ bind(exit);
  }

  // (pre-)fetch invocation count
  if (inc_counter) {
    __ ldr(r2, invocation_counter);
  }

  // And the base dispatch table
  __ mov(rdispatch, (intptr_t)Interpreter::dispatch_table());

  // initialize fixed part of activation frame
  generate_fixed_frame(false, r10);
#ifndef PRODUCT
  // tell the simulator that a method has been entered
  if (NotifySimulator) {
    __ notify(Assembler::method_entry);
  }
#endif
  // make sure method is not native & not abstract
#ifdef ASSERT
  __ ldrw(r0, access_flags);
  {
    Label L;
    __ tst(r0, JVM_ACC_NATIVE);
    __ br(Assembler::EQ, L);
    __ stop("tried to execute native method as non-native");
    __ bind(L);
  }
  {
    Label L;
    __ tst(r0, JVM_ACC_ABSTRACT);
    __ br(Assembler::EQ, L);
    __ stop("tried to execute abstract method in interpreter");
    __ bind(L);
  }
#endif

  // Since at this point in the method invocation the exception
  // handler would try to exit the monitor of synchronized methods
  // which hasn't been entered yet, we set the thread local variable
  // _do_not_unlock_if_synchronized to true. The remove_activation
  // will check this flag.

   const Address do_not_unlock_if_synchronized(rthread,
        in_bytes(JavaThread::do_not_unlock_if_synchronized_offset()));
  __ mov(rscratch2, true);
  __ strb(rscratch2, do_not_unlock_if_synchronized);

  // increment invocation count & check for overflow
  Label invocation_counter_overflow;
  Label profile_method;
  Label profile_method_continue;
  if (inc_counter) {
    generate_counter_incr(&invocation_counter_overflow,
                          &profile_method,
                          &profile_method_continue);
    if (ProfileInterpreter) {
      __ bind(profile_method_continue);
    }
  }

  Label continue_after_compile;
  __ bind(continue_after_compile);

  bang_stack_shadow_pages(false);

  // reset the _do_not_unlock_if_synchronized flag
  __ strb(zr, do_not_unlock_if_synchronized);

  // check for synchronized methods
  // Must happen AFTER invocation_counter check and stack overflow check,
  // so method is not locked if overflows.
  if (synchronized) {
    // Allocate monitor and lock method
    lock_method();
  } else {
    // no synchronization necessary
#ifdef ASSERT
    {
      Label L;
      __ ldrw(r0, access_flags);
      __ tst(r0, JVM_ACC_SYNCHRONIZED);
      __ br(Assembler::EQ, L);
      __ stop("method needs synchronization");
      __ bind(L);
    }
#endif
  }

  // start execution
#ifdef ASSERT
  {
    Label L;
     const Address monitor_block_top (rfp,
                 frame::interpreter_frame_monitor_block_top_offset * wordSize);
    __ ldr(rscratch1, monitor_block_top);
    __ cmp(esp, rscratch1);
    __ br(Assembler::EQ, L);
    __ stop("broken stack frame setup in interpreter");
    __ bind(L);
  }
#endif

  // jvmti support
  __ notify_method_entry();

  __ dispatch_next(vtos);

  // invocation counter overflow
  if (inc_counter) {
    if (ProfileInterpreter) {
      // We have decided to profile this method in the interpreter
      __ bind(profile_method);
      __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::profile_method));
      __ set_method_data_pointer_for_bcp();
      __ get_method(r1);
      __ b(profile_method_continue);
    }
    // Handle overflow of counter and compile method
    __ bind(invocation_counter_overflow);
    generate_counter_overflow(&continue_after_compile);
  }

  return entry_point;
}

// Entry points
//
// Here we generate the various kind of entries into the interpreter.
// The two main entry type are generic bytecode methods and native
// call method.  These both come in synchronized and non-synchronized
// versions but the frame layout they create is very similar. The
// other method entry types are really just special purpose entries
// that are really entry and interpretation all in one. These are for
// trivial methods like accessor, empty, or special math methods.
//
// When control flow reaches any of the entry types for the interpreter
// the following holds ->
//
// Arguments:
//
// rmethod: Method*
//
// Stack layout immediately at entry
//
// [ return address     ] <--- rsp
// [ parameter n        ]
//   ...
// [ parameter 1        ]
// [ expression stack   ] (caller's java expression stack)

// Assuming that we don't go to one of the trivial specialized entries
// the stack will look like below when we are ready to execute the
// first bytecode (or call the native routine). The register usage
// will be as the template based interpreter expects (see
// interpreter_aarch64.hpp).
//
// local variables follow incoming parameters immediately; i.e.
// the return address is moved to the end of the locals).
//
// [ monitor entry      ] <--- esp
//   ...
// [ monitor entry      ]
// [ expr. stack bottom ]
// [ saved rbcp         ]
// [ current rlocals    ]
// [ Method*            ]
// [ saved rfp          ] <--- rfp
// [ return address     ]
// [ local variable m   ]
//   ...
// [ local variable 1   ]
// [ parameter n        ]
//   ...
// [ parameter 1        ] <--- rlocals

address AbstractInterpreterGenerator::generate_method_entry(
                                        AbstractInterpreter::MethodKind kind) {
  // determine code generation flags
  bool synchronized = false;
  address entry_point = NULL;

  switch (kind) {
  case Interpreter::zerolocals             :                                                                             break;
  case Interpreter::zerolocals_synchronized: synchronized = true;                                                        break;
  case Interpreter::native                 : entry_point = ((InterpreterGenerator*) this)->generate_native_entry(false); break;
  case Interpreter::native_synchronized    : entry_point = ((InterpreterGenerator*) this)->generate_native_entry(true);  break;
  case Interpreter::empty                  : entry_point = ((InterpreterGenerator*) this)->generate_empty_entry();       break;
  case Interpreter::accessor               : entry_point = ((InterpreterGenerator*) this)->generate_accessor_entry();    break;
  case Interpreter::abstract               : entry_point = ((InterpreterGenerator*) this)->generate_abstract_entry();    break;
  case Interpreter::method_handle          : entry_point = ((InterpreterGenerator*) this)->generate_method_handle_entry();break;

  case Interpreter::java_lang_math_sin     : // fall thru
  case Interpreter::java_lang_math_cos     : // fall thru
  case Interpreter::java_lang_math_tan     : // fall thru
  case Interpreter::java_lang_math_abs     : // fall thru
  case Interpreter::java_lang_math_log     : // fall thru
  case Interpreter::java_lang_math_log10   : // fall thru
  case Interpreter::java_lang_math_sqrt    : // fall thru
  case Interpreter::java_lang_math_pow     : // fall thru
  case Interpreter::java_lang_math_exp     : entry_point = ((InterpreterGenerator*) this)->generate_math_entry(kind);    break;
  case Interpreter::java_lang_ref_reference_get
                                           : entry_point = ((InterpreterGenerator*)this)->generate_Reference_get_entry(); break;
  default                                  : ShouldNotReachHere();                                                       break;
  }

  if (entry_point) {
    return entry_point;
  }

  return ((InterpreterGenerator*) this)->
                                generate_normal_entry(synchronized);
}


// These should never be compiled since the interpreter will prefer
// the compiled version to the intrinsic version.
bool AbstractInterpreter::can_be_compiled(methodHandle m) { return false; }

// How much stack a method activation needs in words.
int AbstractInterpreter::size_top_interpreter_activation(methodOop method) {
  const int entry_size = frame::interpreter_frame_monitor_size();

  // total overhead size: entry_size + (saved rfp thru expr stack
  // bottom).  be sure to change this if you add/subtract anything
  // to/from the overhead area
  const int overhead_size =
    -(frame::interpreter_frame_initial_sp_offset) + entry_size;

  const int stub_code = frame::entry_frame_after_call_words;
  const int extra_stack = methodOopDesc::extra_stack_entries();
  const int method_stack = (method->max_locals() + method->max_stack() + extra_stack) *
                           Interpreter::stackElementWords;
  return (overhead_size + method_stack + stub_code);
}

int AbstractInterpreter::layout_activation(methodOop method,
                                           int tempcount,
                                           int popframe_extra_args,
                                           int moncount,
                                           int caller_actual_parameters,
                                           int callee_param_count,
                                           int callee_locals,
                                           frame* caller,
                                           frame* interpreter_frame,
                                           bool is_top_frame) { Unimplemented(); return 0; }

//-----------------------------------------------------------------------------
// Exceptions

void TemplateInterpreterGenerator::generate_throw_exception() {
  // Entry point in previous activation (i.e., if the caller was
  // interpreted)
  Interpreter::_rethrow_exception_entry = __ pc();
  // Restore sp to interpreter_frame_last_sp even though we are going
  // to empty the expression stack for the exception processing.
  __ str(zr, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  // r0: exception
  // r3: return address/pc that threw exception
  __ restore_bcp();    // rbcp points to call/send
  __ restore_locals();
  __ restore_constant_pool_cache();
  __ reinit_heapbase();  // restore rheapbase as heapbase.
#ifndef PRODUCT
  // tell the simulator that the caller method has been reentered
  if (NotifySimulator) {
    __ get_method(rmethod);
    __ notify(Assembler::method_reentry);
  }
#endif
  // Entry point for exceptions thrown within interpreter code
  Interpreter::_throw_exception_entry = __ pc();
  // If we came here via a NullPointerException on the receiver of a
  // method, rmethod may be corrupt.
  __ get_method(rmethod);
  // expression stack is undefined here
  // r0: exception
  // rbcp: exception bcp
  __ verify_oop(r0);
  __ mov(c_rarg1, r0);

  // expression stack must be empty before entering the VM in case of
  // an exception
  __ empty_expression_stack();
  // find exception handler address and preserve exception oop
  __ call_VM(r3,
             CAST_FROM_FN_PTR(address,
                          InterpreterRuntime::exception_handler_for_exception),
             c_rarg1);

  // For what we are about to push, make room.
  __ sub(rscratch1, esp, wordSize);
  __ cmp(sp, rscratch1);
  Label enough_room;
  __ br(Assembler::HS, enough_room);
  __ andr(sp, rscratch1, -16);
  __ bind(enough_room);

  // r0: exception handler entry point
  // r3: preserved exception oop
  // rbcp: bcp for exception handler
  __ push_ptr(r3); // push exception which is now the only value on the stack
  __ br(r0); // jump to exception handler (may be _remove_activation_entry!)

  // If the exception is not handled in the current frame the frame is
  // removed and the exception is rethrown (i.e. exception
  // continuation is _rethrow_exception).
  //
  // Note: At this point the bci is still the bxi for the instruction
  // which caused the exception and the expression stack is
  // empty. Thus, for any VM calls at this point, GC will find a legal
  // oop map (with empty expression stack).

  // In current activation
  // tos: exception
  // esi: exception bcp

  //
  // JVMTI PopFrame support
  //

  Interpreter::_remove_activation_preserving_args_entry = __ pc();
  __ empty_expression_stack();
  // Set the popframe_processing bit in pending_popframe_condition
  // indicating that we are currently handling popframe, so that
  // call_VMs that may happen later do not trigger new popframe
  // handling cycles.
  __ ldr(r3, Address(rthread, JavaThread::popframe_condition_offset()));
  __ orr(r3, r3, JavaThread::popframe_processing_bit);
  __ str(r3, Address(rthread, JavaThread::popframe_condition_offset()));

  {
    // Check to see whether we are returning to a deoptimized frame.
    // (The PopFrame call ensures that the caller of the popped frame is
    // either interpreted or compiled and deoptimizes it if compiled.)
    // In this case, we can't call dispatch_next() after the frame is
    // popped, but instead must save the incoming arguments and restore
    // them after deoptimization has occurred.
    //
    // Note that we don't compare the return PC against the
    // deoptimization blob's unpack entry because of the presence of
    // adapter frames in C2.
    Label caller_not_deoptimized;
    __ ldr(c_rarg1, Address(rfp, frame::return_addr_offset * wordSize));
    __ super_call_VM_leaf(CAST_FROM_FN_PTR(address,
                               InterpreterRuntime::interpreter_contains), c_rarg1);
    __ cbnz(r0, caller_not_deoptimized);

    __ call_Unimplemented();

    // Compute size of arguments for saving when returning to
    // deoptimized caller
    __ get_method(r0);
    __ load_unsigned_short(r0, Address(r0, in_bytes(methodOopDesc::
						    size_of_parameters_offset())));
    __ lsl(r0, r0, Interpreter::logStackElementSize);
    __ restore_locals(); // XXX do we need this?
    __ sub(rlocals, rlocals, r0);
    __ add(rlocals, rlocals, wordSize);
    // Save these arguments
    __ super_call_VM_leaf(CAST_FROM_FN_PTR(address,
                                           Deoptimization::
                                           popframe_preserve_args),
                          rthread, r0, rlocals);

    __ remove_activation(vtos,
                         /* throw_monitor_exception */ false,
                         /* install_monitor_exception */ false,
                         /* notify_jvmdi */ false);

    // Inform deoptimization that it is responsible for restoring
    // these arguments
    __ mov(rscratch1, JavaThread::popframe_force_deopt_reexecution_bit);
    __ strw(rscratch1, Address(rthread, JavaThread::popframe_condition_offset()));

    // Continue in deoptimization handler
    __ ret(lr);

    __ bind(caller_not_deoptimized);
  }

  __ remove_activation(vtos,
                       /* throw_monitor_exception */ false,
                       /* install_monitor_exception */ false,
                       /* notify_jvmdi */ false);

  // Finish with popframe handling
  // A previous I2C followed by a deoptimization might have moved the
  // outgoing arguments further up the stack. PopFrame expects the
  // mutations to those outgoing arguments to be preserved and other
  // constraints basically require this frame to look exactly as
  // though it had previously invoked an interpreted activation with
  // no space between the top of the expression stack (current
  // last_sp) and the top of stack. Rather than force deopt to
  // maintain this kind of invariant all the time we call a small
  // fixup routine to move the mutated arguments onto the top of our
  // expression stack if necessary.
  __ mov(c_rarg1, sp);
  __ ldr(c_rarg2, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  // PC must point into interpreter here
  __ set_last_Java_frame(noreg, rfp, __ pc());
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, InterpreterRuntime::popframe_move_outgoing_args), rthread, c_rarg1, c_rarg2);
  __ reset_last_Java_frame(true, true);
  // Restore the last_sp and null it out
  __ ldr(rscratch1, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));
  __ mov(esp, rscratch1);
  __ str(zr, Address(rfp, frame::interpreter_frame_last_sp_offset * wordSize));

  __ restore_bcp();  // XXX do we need this?
  __ restore_locals(); // XXX do we need this?
  __ restore_constant_pool_cache();
  __ get_method(rmethod);

  // The method data pointer was incremented already during
  // call profiling. We have to restore the mdp for the current bcp.
  if (ProfileInterpreter) {
    __ set_method_data_pointer_for_bcp();
  }

  // Clear the popframe condition flag
  __ str(zr, Address(rthread, JavaThread::popframe_condition_offset()));
  assert(JavaThread::popframe_inactive == 0, "fix popframe_inactive");

  __ dispatch_next(vtos);
  // end of PopFrame support

  Interpreter::_remove_activation_entry = __ pc();

  // preserve exception over this code sequence
  __ pop_ptr(r0);
  __ str(r0, Address(rthread, JavaThread::vm_result_offset()));
  // remove the activation (without doing throws on illegalMonitorExceptions)
  __ remove_activation(vtos, false, true, false);
  // restore exception
  __ ldr(r0, Address(rthread, JavaThread::vm_result_offset()));
  __ str(zr, Address(rthread, JavaThread::vm_result_offset()));
  __ verify_oop(r0);

  // In between activations - previous activation type unknown yet
  // compute continuation point - the continuation point expects the
  // following registers set up:
  //
  // r0: exception
  // lr: return address/pc that threw exception
  // rsp: expression stack of caller
  // rfp: fp of caller
  // FIXME: There's no point saving LR here because VM calls don't trash it
  __ stp(r0, lr, Address(__ pre(sp, -2 * wordSize)));  // save exception & return address
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address,
                          SharedRuntime::exception_handler_for_return_address),
                        rthread, lr);
  __ mov(r1, r0);                               // save exception handler
  __ ldp(r0, lr, Address(__ post(sp, 2 * wordSize)));  // restore exception & return address
  // Note that an "issuing PC" is actually the next PC after the call
  __ br(r1);                                    // jump to exception
                                                // handler of caller
}


//
// JVMTI ForceEarlyReturn support
//
address TemplateInterpreterGenerator::generate_earlyret_entry_for(TosState state) { __ call_Unimplemented(); return 0; }
// end of ForceEarlyReturn support


//-----------------------------------------------------------------------------
// Helper for vtos entry point generation

void TemplateInterpreterGenerator::set_vtos_entry_points(Template* t,
                                                         address& bep,
                                                         address& cep,
                                                         address& sep,
                                                         address& aep,
                                                         address& iep,
                                                         address& lep,
                                                         address& fep,
                                                         address& dep,
                                                         address& vep) {
  assert(t->is_valid() && t->tos_in() == vtos, "illegal template");
  Label L;
  aep = __ pc();  __ push_ptr();  __ b(L);
  fep = __ pc();  __ push_f();    __ b(L);
  dep = __ pc();  __ push_d();    __ b(L);
  lep = __ pc();  __ push_l();    __ b(L);
  bep = cep = sep =
  iep = __ pc();  __ push_i();
  vep = __ pc();
  __ bind(L);
  generate_and_dispatch(t);
}

//-----------------------------------------------------------------------------
// Generation of individual instructions

// helpers for generate_and_dispatch


InterpreterGenerator::InterpreterGenerator(StubQueue* code)
  : TemplateInterpreterGenerator(code) {
   generate_all(); // down here so it can be "virtual"
}

//-----------------------------------------------------------------------------

// Non-product code
#ifndef PRODUCT
address TemplateInterpreterGenerator::generate_trace_code(TosState state) {
  address entry = __ pc();

  __ push(lr);
  __ push(state);
  __ push(0xffffu, sp);
  __ mov(c_rarg2, r0);  // Pass itos
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, SharedRuntime::trace_bytecode),
             c_rarg1, c_rarg2, c_rarg3);
  __ pop(0xffffu, sp);
  __ pop(state);
  __ pop(lr);
  __ ret(lr);                                   // return from result handler

  return entry;
}

void TemplateInterpreterGenerator::count_bytecode() {
  __ push(rscratch1);
  __ push(rscratch2);
  Label L;
  __ mov(rscratch2, (address) &BytecodeCounter::_counter_value);
  __ bind(L);
  __ ldxr(rscratch1, rscratch2);
  __ add(rscratch1, rscratch1, 1);
  __ stxr(rscratch1, rscratch1, rscratch2);
  __ cbnzw(rscratch1, L);
  __ pop(rscratch2);
  __ pop(rscratch1);
}

void TemplateInterpreterGenerator::histogram_bytecode(Template* t) { ; }

void TemplateInterpreterGenerator::histogram_bytecode_pair(Template* t) { ; }


void TemplateInterpreterGenerator::trace_bytecode(Template* t) {
  // Call a little run-time stub to avoid blow-up for each bytecode.
  // The run-time runtime saves the right registers, depending on
  // the tosca in-state for the given template.

  assert(Interpreter::trace_code(t->tos_in()) != NULL,
         "entry must have been generated");
  __ bl(Interpreter::trace_code(t->tos_in()));
  __ reinit_heapbase();
}


void TemplateInterpreterGenerator::stop_interpreter_at() {
  Label L;
  __ push(rscratch1);
  __ mov(rscratch1, (address) &BytecodeCounter::_counter_value);
  __ ldr(rscratch1, Address(rscratch1));
  __ mov(rscratch2, StopInterpreterAt);
  __ cmpw(rscratch1, rscratch2);
  __ br(Assembler::NE, L);
  __ brk(0);
  __ bind(L);
  __ pop(rscratch1);
}

#include <sys/mman.h>
#include <unistd.h>

extern "C" {
  static int PAGESIZE = getpagesize();
  int is_mapped_address(u_int64_t address)
  {
    address = (address & ~((u_int64_t)PAGESIZE - 1));
    if (msync((void *)address, PAGESIZE, MS_ASYNC) == 0) {
      return true;
    }
    if (errno != ENOMEM) {
      return true;
    }
    return false;
  }

  void bccheck1(u_int64_t methodVal, u_int64_t bcpVal, int verify, char *method, int *bcidx, char *decode)
  {
    if (method != 0) {
      method[0] = '\0';
    }
    if (bcidx != 0) {
      *bcidx = -2;
    }
    if (decode != 0) {
      decode[0] = 0;
    }

    if (verify) {
      if (!is_mapped_address(methodVal)) {
	return;
      }
      Metadata *md = (Metadata*)methodVal;
      if (!md->is_valid()) {
	return;
      }
      if (!md->is_method()) {
	return;
      }
    }
    if ((checkVal & Universe::verify_klass_mask()) != Universe::verify_klass_bits()) {
      return;
    }
    // is the 'klassOop' of the 'klassOop' in range
    checkVal = (intptr_t)((klassOop)checkVal)->klass();
    if (checkVal == 0) {
      return;
    }
    if ((checkVal & Universe::verify_klass_mask()) != Universe::verify_klass_bits()) {
      return;
    }
    methodOop meth = (methodOop)methodVal;
    address bcp = (address)bcpVal;
    if (method) {
     meth->name_and_sig_as_C_string(method, 400);
    }
    if (bcidx) {
     if (meth->contains(bcp)) {
    	*bcidx = meth->bci_from(bcp);
     } else {
    	*bcidx = -2;
     }
    }
    if (decode) {
      if (!BytecodeTracer::closure()) {
	BytecodeTracer::set_closure(BytecodeTracer::std_closure());
      }
      stringStream str(decode, 400);
      BytecodeTracer::trace(meth, bcp, &str);
    }
  }


  JNIEXPORT void bccheck(u_int64_t methodVal, u_int64_t bcpVal, int verify, char *method, int *bcidx, char *decode)
  {
    if (! ((Method *)methodVal)->is_native())
      bccheck1(methodVal, bcpVal, verify, method, bcidx, decode);
  }
}

#endif // !PRODUCT
#endif // ! CC_INTERP
