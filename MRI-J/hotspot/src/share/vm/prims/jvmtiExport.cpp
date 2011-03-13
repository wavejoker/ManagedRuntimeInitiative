/*
 * Copyright 2003-2007 Sun Microsystems, Inc.  All Rights Reserved.
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
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *  
 */
// This file is a derivative work resulting from (and including) modifications
// made by Azul Systems, Inc.  The date of such changes is 2010.
// Copyright 2010 Azul Systems, Inc.  All Rights Reserved.
//
// Please contact Azul Systems, Inc., 1600 Plymouth Street, Mountain View, 
// CA 94043 USA, or visit www.azulsystems.com if you need additional information 
// or have any questions.


#include "allocation.hpp"
#include "arguments.hpp"
#include "attachListener.hpp"
#include "collectedHeap.hpp"
#include "fieldDescriptor.hpp"
#include "gcLocker.hpp"
#include "genCollectedHeap.hpp"
#include "gpgc_heap.hpp"
#include "hpi.hpp"
#include "interfaceSupport.hpp"
#include "javaClasses.hpp"
#include "jniHandles.hpp"
#include "jvm_os.h"
#include "jvmtiCodeBlobEvents.hpp"
#include "jvmtiEnvBase.hpp"
#include "jvmtiEnvThreadState.hpp"
#include "jvmtiExport.hpp"
#include "jvmtiImpl.hpp"
#include "jvmtiManageCapabilities.hpp"
#include "jvmtiTagMap.hpp"
#include "jvmtiThreadState.hpp"
#include "psMarkSweep.hpp"
#include "psParallelCompact.hpp"
#include "resourceArea.hpp"
#include "safepoint.hpp"
#include "serviceUtil.hpp"
#include "stubRoutines.hpp"
#include "surrogateLockerThread.hpp"
#include "systemDictionary.hpp"
#include "thread.hpp"
#include "tickProfiler.hpp"
#include "vframe.hpp"
#include "vmThread.hpp"

#include "atomic_os_pd.inline.hpp"
#include "frame.inline.hpp"
#include "handles.inline.hpp"
#include "jvmtiEventController.inline.hpp"
#include "jvmtiThreadState.inline.hpp"
#include "mutex.inline.hpp"
#include "oop.inline.hpp"
#include "orderAccess_os_pd.inline.hpp"
#include "thread_os.inline.hpp"

#ifdef AZ_PROXIED
#include <proxy/proxy_java.h>
#endif // AZ_PROXIED

// Machine generated file; must come last:
#include "jvmtiEnv.hpp"

#ifdef JVMTI_TRACE
#define EVT_TRACE(evt,out) if ((JvmtiTrace::event_trace_flags(evt) & JvmtiTrace::SHOW_EVENT_SENT) != 0) { SafeResourceMark rm; tty->print_cr out; }
#define EVT_TRIG_TRACE(evt,out) if ((JvmtiTrace::event_trace_flags(evt) & JvmtiTrace::SHOW_EVENT_TRIGGER) != 0) { SafeResourceMark rm; tty->print_cr out; }
#else
#define EVT_TRIG_TRACE(evt,out)
#define EVT_TRACE(evt,out)
#endif


///////////////////////////////////////////////////////////////
//
// JvmtiEventTransition
//
// TO DO --
//  more handle purging

// Use this for JavaThreads and state is  _thread_in_vm.
class JvmtiJavaThreadEventTransition : StackObj {
private:
  ResourceMark _rm;
  ThreadToNativeFromVM _transition;
  HandleMark _hm;  

public:
  JvmtiJavaThreadEventTransition(JavaThread *thread) : 
    _rm(),
    _transition(thread),
    _hm(thread)  {};
};

// For JavaThreads which are not in _thread_in_vm state
// and other system threads use this.
class JvmtiThreadEventTransition : StackObj {
private:
  ResourceMark _rm;
  HandleMark _hm;
  JavaThread *_jthread;
    
public:
  JvmtiThreadEventTransition(Thread *thread) : _rm(), _hm() {
    if (thread->is_Java_thread()) {
      _jthread = (JavaThread *)thread;

      // This code matches ThreadToNativeFromVM
      // TODO Change ThreadToNativeFromVM to handle Thread instead of JavaThread

      assert0( _jthread->_allow_safepoint_count == 0 );
assert(_jthread!=NULL&&_jthread->is_Java_thread(),"must be Java thread");
      _jthread->verify_anchor();
      // We are leaving the VM at this point and going directly to native code. 
assert(!_jthread->owns_locks(),"must release all locks when leaving VM");
      
      // Try to park right before giving up jvm lock
      if (ParkTLAB) {
        _jthread->tlab().park();
      }
      // Release the JVM lock when back in the native code
      _jthread->jvm_unlock_self(); // Fast & easy!
      // VM code can set a pending exception.  Native code will ignore it, but
      // when the native code transitions back to Java code, the Java code needs
      // to handle it.
      if( _jthread->has_pending_exception() ) // If there was a pending exception
        _jthread->set_jvm_lock_poll_advisory(); // notice it before retaking the jvm lock
      // No safepoint polling required, its optional.

      // No async exception checking: these are already installed in
      // _pending_async_exception and will appear at the next poll.

      // This means we can enter native code with _pending_async_exception
      // already set.
    } else {
      _jthread = NULL;
    }
  }
    
  ~JvmtiThreadEventTransition() {

    // This code matches ~ThreadToNativeFromVM
    // TODO Change ThreadToNativeFromVM to handle Thread instead of JavaThread

    if (_jthread) {
      // Take the JVM lock (so the VM can handle naked oops) or block in the attempt
      if( _jthread->please_self_suspend() ) // Inline 1 check
        _jthread->jvm_lock_self_or_suspend(); // Poll & handle funny conditions, then relock
      else
        _jthread->jvm_lock_self();
      assert0( _jthread->_allow_safepoint_count == 0 );
    }
  }
};


///////////////////////////////////////////////////////////////
//
// JvmtiEventMark
//

class JvmtiEventMark : public StackObj {
private:
  JavaThread *_thread;
  JNIEnv* _jni_env;
  bool _exception_detected;   
  bool _exception_caught;
#if 0
  JNIHandleBlock* _hblock;
#endif

public:
  JvmtiEventMark(JavaThread *thread) :  _thread(thread),
                                         _jni_env(thread->jni_environment()) {
#if 0
    _hblock = thread->active_handles();
    _hblock->clear_thoroughly(); // so we can be safe
#else
    // we want to use the code above - but that needs the JNIHandle changes - later...
    // for now, steal JNI push local frame code
    JvmtiThreadState *state = thread->jvmti_thread_state();
    // we are before an event.
    // Save current jvmti thread exception state. 
    if (state != NULL) {
      _exception_detected = state->is_exception_detected();
      _exception_caught = state->is_exception_caught();
    } else {
      _exception_detected = false;
      _exception_caught = false;
    }
    
    JNIHandleBlock* old_handles = thread->active_handles();
    JNIHandleBlock* new_handles = JNIHandleBlock::allocate_block(thread);
    assert(new_handles != NULL, "should not be NULL");
    new_handles->set_pop_frame_link(old_handles);
    thread->set_active_handles(new_handles);
#endif
    assert(thread == JavaThread::current(), "thread must be current!");
thread->make_stack_coherent();
  };

  // Azul - Make this a virtual so that the derived destructors get called correctly
  virtual ~JvmtiEventMark() {
#if 0
    _hblock->clear(); // for consistency with future correct behavior
#else
    // we want to use the code above - but that needs the JNIHandle changes - later...
    // for now, steal JNI pop local frame code
    JNIHandleBlock* old_handles = _thread->active_handles();
    JNIHandleBlock* new_handles = old_handles->pop_frame_link();
    assert(new_handles != NULL, "should not be NULL");
    _thread->set_active_handles(new_handles);
    // Note that we set the pop_frame_link to NULL explicitly, otherwise
    // the release_block call will release the blocks.
    old_handles->set_pop_frame_link(NULL); 
    JNIHandleBlock::release_block(old_handles, _thread); // may block
#endif

    JvmtiThreadState* state = _thread->jvmti_thread_state();
    // we are continuing after an event.
    if (state != NULL) {
      // Restore the jvmti thread exception state.
      if (_exception_detected) {
        state->set_exception_detected();
      }
      if (_exception_caught) {
        state->set_exception_caught();
      }
    }
  }      
  
#if 0
  jobject to_jobject(oop obj) { return obj == NULL? NULL : _hblock->allocate_handle_fast(obj); }
#else
  // we want to use the code above - but that needs the JNIHandle changes - later...
  // for now, use regular make_local
  jobject to_jobject(oop obj) { return JNIHandles::make_local(_thread,obj); }
#endif

  jclass to_jclass(klassOop klass) { return (klass == NULL ? NULL : (jclass)to_jobject(Klass::cast(klass)->java_mirror())); }

  jmethodID to_jmethodID(methodHandle method) { return method->jmethod_id(); }

  JNIEnv* jni_env() { return _jni_env; }
};

class JvmtiThreadEventMark : public JvmtiEventMark {
private:
  jthread _jt;

public:
  JvmtiThreadEventMark(JavaThread *thread) : 
    JvmtiEventMark(thread) {
    _jt = (jthread)(to_jobject(thread->threadObj()));
  };

  // Azul - destroy the JNI handle allocated above
  ~JvmtiThreadEventMark() {
    // Insert NULL instead of calling destroy_local because the deleted_handle can
    // be moved by GC and this will point to garbage - bug# 15339
if(_jt!=NULL){
      *(intptr_t*)_jt = NULL;
    }
  }

 jthread jni_thread() { return _jt; }
};

class JvmtiClassEventMark : public JvmtiThreadEventMark {
private:
  jclass _jc;

public:
  JvmtiClassEventMark(JavaThread *thread, klassOop klass) : 
    JvmtiThreadEventMark(thread) {
    _jc = to_jclass(klass);
  };

  // Azul - destroy the JNI handle allocated above
  ~JvmtiClassEventMark() {
    // Insert NULL instead of calling destroy_local because the deleted_handle can
    // be moved by GC and this will point to garbage - bug# 15339
if(_jc!=NULL){
      *(intptr_t*)_jc = NULL;
    }
  }

  jclass jni_class() { return _jc; }
};

class JvmtiMethodEventMark : public JvmtiThreadEventMark {
private:
  jmethodID _mid;

public:
  JvmtiMethodEventMark(JavaThread *thread, methodHandle method) : 
    JvmtiThreadEventMark(thread),
    _mid(to_jmethodID(method)) {};
  jmethodID jni_methodID() { return _mid; }
};

class JvmtiLocationEventMark : public JvmtiMethodEventMark {
private:
  jlocation _loc;

public:
  JvmtiLocationEventMark(JavaThread *thread, methodHandle method, address location) : 
    JvmtiMethodEventMark(thread, method),
    _loc(location - method->code_base()) {};
  jlocation location() { return _loc; }
};

class JvmtiExceptionEventMark : public JvmtiLocationEventMark {
private:
  jobject _exc;

public:
  JvmtiExceptionEventMark(JavaThread *thread, methodHandle method, address location, Handle exception) : 
    JvmtiLocationEventMark(thread, method, location),
    _exc(to_jobject(exception())) {};
  jobject exception() { return _exc; }
};

class JvmtiClassFileLoadEventMark : public JvmtiThreadEventMark {
private:
  const char *_class_name;
  jobject _jloader;
  jobject _protection_domain;
  jclass  _class_being_redefined;

public:
  JvmtiClassFileLoadEventMark(JavaThread *thread, symbolHandle name,
     Handle class_loader, Handle prot_domain, KlassHandle *class_being_redefined) : JvmtiThreadEventMark(thread) {
      _class_name = name() != NULL? name->as_utf8() : NULL;
      _jloader = (jobject)to_jobject(class_loader());
      _protection_domain = (jobject)to_jobject(prot_domain());
      if (class_being_redefined == NULL) {
	_class_being_redefined = NULL;
      } else {
	_class_being_redefined = (jclass)to_jclass((*class_being_redefined)());
      }
  };
  const char *class_name() {
    return _class_name;
  }
  jobject jloader() {
    return _jloader;
  }
  jobject protection_domain() {
    return _protection_domain;
  }
  jclass class_being_redefined() {
    return _class_being_redefined;
  }
};

static inline klassOop oop_to_klassOop(oop obj) {
  klassOop k = obj->klass();

  // if the object is a java.lang.Class then return the java mirror
  if (k == SystemDictionary::class_klass()) {
    if (!java_lang_Class::is_primitive(obj)) {
      k = java_lang_Class::as_klassOop(obj);
      assert(k != NULL, "class for non-primitive mirror must exist");      
    }
  }  
  return k;
}

class JvmtiVMObjectAllocEventMark : public JvmtiClassEventMark  {
 private:
   jobject _jobj;
   jlong    _size;
 public:
   JvmtiVMObjectAllocEventMark(JavaThread *thread, oop obj) : JvmtiClassEventMark(thread, oop_to_klassOop(obj)) {
     _jobj = (jobject)to_jobject(obj);
     _size = obj->size() * wordSize;
   };
   jobject jni_jobject() { return _jobj; }
   jlong size() { return _size; }
};

class JvmtiCompiledMethodLoadEventMark : public JvmtiMethodEventMark {
 private:
  jint _code_size;
  const void *_code_data;
  jint _map_length;
  jvmtiAddrLocationMap *_map;
  const void *_compile_info;
 public:
JvmtiCompiledMethodLoadEventMark(JavaThread*thread,methodCodeHandle*nmch)
          : JvmtiMethodEventMark(thread,methodHandle((*nmch)()->method())) {
    _code_data = (*nmch)()->_blob->code_begins();
    _code_size = (*nmch)()->_blob->code_size();
    _compile_info = NULL; /* no info for our VM. */
JvmtiCodeBlobEvents::build_jvmti_addr_location_map((*nmch)()->_blob,&_map,&_map_length);
  }
  ~JvmtiCompiledMethodLoadEventMark() {
     FREE_C_HEAP_ARRAY(jvmtiAddrLocationMap, _map);
  }

  jint code_size() { return _code_size; }
  const void *code_data() { return _code_data; }
  jint map_length() { return _map_length; }
  const jvmtiAddrLocationMap* map() { return _map; }
  const void *compile_info() { return _compile_info; }
};

class JvmtiMonitorEventMark : public JvmtiThreadEventMark {
private:
  jobject _jobj;
public:
  JvmtiMonitorEventMark(JavaThread *thread, oop object) 
          : JvmtiThreadEventMark(thread){
     _jobj = to_jobject(object);
  }
  jobject jni_object() { return _jobj; }
};

///////////////////////////////////////////////////////////////
//
// pending CompiledMethodUnload support
//

bool JvmtiExport::_have_pending_compiled_method_unload_events;		
GrowableArray<jmethodID>* JvmtiExport::_pending_compiled_method_unload_method_ids;	
GrowableArray<const void *>* JvmtiExport::_pending_compiled_method_unload_code_begins;	
JavaThread* JvmtiExport::_current_poster;

// post any pending CompiledMethodUnload events

void JvmtiExport::post_pending_compiled_method_unload_events() {
  JavaThread* self = JavaThread::current();
  assert(!self->owns_locks(), "can't hold locks");

  // Indicates if this is the first activiation of this function.
  // In theory the profiler's callback could call back into VM and provoke
  // another CompiledMethodLoad event to be posted from this thread. As the
  // stack rewinds we need to ensure that the original activation does the
  // completion and notifies any waiters.
  bool first_activation = false;

  // the jmethodID (may not be valid) to be used for a single event
  jmethodID method;
  const void *code_begin;

  // grab the monitor and check if another thread is already posting
  // events. If there is another thread posting events then we wait
  // until it completes. (In theory we could check the pending events to
  // see if any of the addresses overlap with the event that we want to
  // post but as it will happen so rarely we just block any thread waiting
  // to post a CompiledMethodLoad or DynamicCodeGenerated event until all
  // pending CompiledMethodUnload events have been posted).
  //
  // If another thread isn't posting we examine the list of pending jmethodIDs.
  // If the list is empty then we are done. If it's not empty then this thread
  // (self) becomes the pending event poster and we remove the top (last) 
  // event from the list. Note that this means we remove the newest event first
  // but as they are all CompiledMethodUnload events the order doesn't matter.
  // Once we have removed a jmethodID then we exit the monitor. Any other thread
  // wanting to post a CompiledMethodLoad or DynamicCodeGenerated event will 
  // be forced to wait on the monitor.
  {
    MutexLocker mu(JvmtiPendingEvent_lock);
    if (_current_poster != self) {
      while (_current_poster != NULL) {
JvmtiPendingEvent_lock.wait();
      }
    }     
    if ((_pending_compiled_method_unload_method_ids == NULL) || 
        (_pending_compiled_method_unload_method_ids->length() == 0)) {
      return;
    }
    if (_current_poster == NULL) {
      _current_poster = self;
      first_activation = true;
    } else {
      // re-entrant
      guarantee(_current_poster == self, "checking");
    }
    method = _pending_compiled_method_unload_method_ids->pop();     
    code_begin = _pending_compiled_method_unload_code_begins->pop();     
  }

  // This thread is the pending event poster so it first posts the CompiledMethodUnload
  // event for the jmethodID that has been removed from the list. Once posted it
  // re-grabs the monitor and checks the list again. If the list is empty then and this
  // is the first activation of the function then we reset the _have_pending_events
  // flag, cleanup _current_poster to indicate that no thread is now servicing the
  // pending events list, and finally notify any thread that might be waiting.
  for (;;) {
    EVT_TRIG_TRACE(JVMTI_EVENT_COMPILED_METHOD_UNLOAD,
                   ("JVMTI [%s] method compile unload event triggered",  
                   JvmtiTrace::safe_get_thread_name(self)));

    // post the event for each environment that has this event enabled.
    JvmtiEnvIterator it; 
    for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
      if (env->is_enabled(JVMTI_EVENT_COMPILED_METHOD_UNLOAD)) {
        EVT_TRACE(JVMTI_EVENT_COMPILED_METHOD_UNLOAD,
                  ("JVMTI [%s] class compile method unload event sent jmethodID " PTR_FORMAT, 
                  JvmtiTrace::safe_get_thread_name(self), method));
 
        JvmtiEventMark jem(self);
        JvmtiJavaThreadEventTransition jet(self);
        jvmtiEventCompiledMethodUnload callback = env->callbacks()->CompiledMethodUnload;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), method, code_begin);
        }
      }
    }

    // event posted, now re-grab monitor and get the next event
    // If there's no next event then we are done. If this is the first
    // activiation of this function by this thread notify any waiters
    // so that they can post.
    {
      MutexLocker ml(JvmtiPendingEvent_lock);      
      if (_pending_compiled_method_unload_method_ids->length() == 0) {
	if (first_activation) {
	  _have_pending_compiled_method_unload_events = false;
	  _current_poster = NULL;
JvmtiPendingEvent_lock.notify_all();
	}
	return;
      }
      method = _pending_compiled_method_unload_method_ids->pop();    
      code_begin = _pending_compiled_method_unload_code_begins->pop();
    }
  }
}

///////////////////////////////////////////////////////////////
//
// JvmtiExport
//

void JvmtiExport::post_raw_breakpoint(JavaThread *thread, methodOop method, address location) {
  HandleMark hm(thread);
  methodHandle mh(thread, method);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  EVT_TRIG_TRACE(JVMTI_EVENT_BREAKPOINT, ("JVMTI [%s] Trg Breakpoint triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    ets->compare_and_set_current_location(mh(), location, JVMTI_EVENT_BREAKPOINT);
    if (!ets->breakpoint_posted() && ets->is_enabled(JVMTI_EVENT_BREAKPOINT)) {
EVT_TRACE(JVMTI_EVENT_BREAKPOINT,("JVMTI [%s] Evt Breakpoint sent %s.%s @ %ld",
                     JvmtiTrace::safe_get_thread_name(thread),
                     (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                     (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                     location - mh()->code_base() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiLocationEventMark jem(thread, mh, location);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventBreakpoint callback = env->callbacks()->Breakpoint;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                    jem.jni_methodID(), jem.location());
      }

      ets->set_breakpoint_posted();
    }
  }
}

jint  
JvmtiExport::get_jvmti_interface(JavaVM *jvm, void **penv, jint version) {
  /* To Do: add version checks */

  // We have to patch the stub routines for monitorenter, monitorexit
  // so they pick up the instrumentation we need.
  StubRoutines::set_should_install_instrumented_stubs(true);
  
  if (JvmtiEnv::get_phase() == JVMTI_PHASE_LIVE) {
JavaThread*current_thread=JavaThread::current();
    // transition code: native to VM
    ThreadInVMfromNative __tiv(current_thread);
    __ENTRY(jvmtiEnv*, JvmtiExport::get_jvmti_interface, (), current_thread)
    debug_only(VMNativeEntryWrapper __vew;) 

    JvmtiEnv *jvmti_env = JvmtiEnv::create_a_jvmti();
    *penv = jvmti_env->jvmti_external();  // actual type is jvmtiEnv* -- not to be confused with JvmtiEnv*
    return JNI_OK; 

  } else if (JvmtiEnv::get_phase() == JVMTI_PHASE_ONLOAD) {
    // not live, no thread to transition
    JvmtiEnv *jvmti_env = JvmtiEnv::create_a_jvmti();
    *penv = jvmti_env->jvmti_external();  // actual type is jvmtiEnv* -- not to be confused with JvmtiEnv*
    return JNI_OK; 

  } else {
    // Called at the wrong time
    *penv = NULL;
    return JNI_EDETACHED;
  }
}


//////////////////////////////////////////////////////////////////////////////

int               JvmtiExport::_field_access_count                        = 0;
int               JvmtiExport::_field_modification_count                  = 0;

bool              JvmtiExport::_can_get_source_debug_extension            = false;
bool              JvmtiExport::_can_examine_or_deopt_anywhere             = false;
bool              JvmtiExport::_can_maintain_original_method_order        = false;
bool              JvmtiExport::_can_post_interpreter_events               = false;
bool JvmtiExport::_can_hotswap_or_post_breakpoint=false;
bool JvmtiExport::_can_modify_any_class=false;
bool JvmtiExport::_can_walk_any_space=false;
bool JvmtiExport::_can_access_local_variables=false;
bool              JvmtiExport::_can_post_exceptions                       = false;
bool              JvmtiExport::_can_post_breakpoint                       = false;
bool              JvmtiExport::_can_post_field_access                     = false;
bool              JvmtiExport::_can_post_field_modification               = false;
bool              JvmtiExport::_can_post_method_entry                     = false;
bool              JvmtiExport::_can_post_method_exit                      = false;
bool              JvmtiExport::_can_pop_frame                             = false;
bool              JvmtiExport::_can_force_early_return                    = false;

bool              JvmtiExport::_should_post_single_step                   = false;
bool              JvmtiExport::_should_post_field_access                  = false;
bool              JvmtiExport::_should_post_field_modification            = false;
bool              JvmtiExport::_should_post_class_load                    = false;
bool              JvmtiExport::_should_post_class_prepare                 = false;
bool              JvmtiExport::_should_post_class_unload                  = false;
bool              JvmtiExport::_should_post_thread_life                   = false;
bool              JvmtiExport::_should_clean_up_heap_objects              = false;
bool              JvmtiExport::_should_post_class_file_load_hook          = false;
bool              JvmtiExport::_should_post_native_method_bind            = false;
bool              JvmtiExport::_should_post_dynamic_code_generated        = false;
bool              JvmtiExport::_should_post_data_dump                     = false;
bool              JvmtiExport::_should_post_compiled_method_load          = false;
bool              JvmtiExport::_should_post_compiled_method_unload        = false;
bool              JvmtiExport::_should_post_monitor_contended_enter       = false;
bool              JvmtiExport::_should_post_monitor_contended_entered     = false;
bool              JvmtiExport::_should_post_monitor_wait                  = false;
bool              JvmtiExport::_should_post_monitor_waited                = false;
bool              JvmtiExport::_should_post_garbage_collection_start      = false;
bool              JvmtiExport::_should_post_garbage_collection_finish     = false;
bool		  JvmtiExport::_should_post_object_free			  = false;
bool		  JvmtiExport::_should_post_resource_exhausted        	  = false;
bool              JvmtiExport::_should_post_vm_object_alloc               = false;
bool              JvmtiExport::_has_redefined_a_class                     = false;
bool		  JvmtiExport::_all_dependencies_are_recorded		  = false;


void JvmtiExport::enter_primordial_phase() { 
  JvmtiEnvBase::set_phase(JVMTI_PHASE_PRIMORDIAL);
}

void JvmtiExport::enter_start_phase() { 
  JvmtiManageCapabilities::recompute_always_capabilities();
  JvmtiEnvBase::set_phase(JVMTI_PHASE_START);
}

void JvmtiExport::enter_onload_phase() { 
  JvmtiEnvBase::set_phase(JVMTI_PHASE_ONLOAD);
}

void JvmtiExport::enter_live_phase() { 
  JvmtiEnvBase::set_phase(JVMTI_PHASE_LIVE);
}



////////////////////////////////////////////////////////////////////////////////////////////////


//
// field access management
//

// interpreter generator needs the address of the counter
address JvmtiExport::get_field_access_count_addr() {
  // We don't grab a lock because we don't want to
  // serialize field access between all threads. This means that a
  // thread on another processor can see the wrong count value and
  // may either miss making a needed call into post_field_access()
  // or will make an unneeded call into post_field_access(). We pay
  // this price to avoid slowing down the VM when we aren't watching
  // field accesses.
  // Other access/mutation safe by virtue of being in VM state.
  return (address)(&_field_access_count);
}


//
// field modification management
//

// interpreter generator needs the address of the counter
address JvmtiExport::get_field_modification_count_addr() {
  // We don't grab a lock because we don't
  // want to serialize field modification between all threads. This
  // means that a thread on another processor can see the wrong
  // count value and may either miss making a needed call into
  // post_field_modification() or will make an unneeded call into
  // post_field_modification(). We pay this price to avoid slowing
  // down the VM when we aren't watching field modifications.
  // Other access/mutation safe by virtue of being in VM state.
  return (address)(&_field_modification_count);
}


//
// JVMTI single step management
//
void JvmtiExport::at_single_stepping_point(JavaThread *thread, methodOop method, address location) {
  assert(JvmtiExport::should_post_single_step(), "must be single stepping");

  HandleMark hm(thread);
  methodHandle mh(thread, method);

  // update information about current location and post a step event
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  EVT_TRIG_TRACE(JVMTI_EVENT_SINGLE_STEP, ("JVMTI [%s] Trg Single Step triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  if (!state->hide_single_stepping()) {
    if (state->is_pending_step_for_popframe()) {
      state->process_pending_step_for_popframe();
    }
    if (state->is_pending_step_for_earlyret()) {
      state->process_pending_step_for_earlyret();
    }
    JvmtiExport::post_single_step(thread, mh(), location);
  }
}


void JvmtiExport::expose_single_stepping(JavaThread *thread) {
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state != NULL) {
    state->clear_hide_single_stepping();
  }
}


bool JvmtiExport::hide_single_stepping(JavaThread *thread) {
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state != NULL && state->is_enabled(JVMTI_EVENT_SINGLE_STEP)) { 
    state->set_hide_single_stepping();
    return true; 
  } else {
    return false; 
  }
}

//
// JVMTI events that the VM posts to the debugger and also startup agent
// and call the agent's premain() for java.lang.instrument.
//

void JvmtiExport::post_vm_start() {
  EVT_TRIG_TRACE(JVMTI_EVENT_VM_START, ("JVMTI Trg VM start event triggered" ));

  // can now enable some events 
  JvmtiEventController::vm_start();

  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_VM_START)) {
      EVT_TRACE(JVMTI_EVENT_VM_START, ("JVMTI Evt VM start event sent" ));

      JavaThread *thread  = JavaThread::current();
      JvmtiThreadEventMark jem(thread);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventVMStart callback = env->callbacks()->VMStart;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env());
      }
    }
  }  
}


void JvmtiExport::post_vm_initialized() {
  EVT_TRIG_TRACE(JVMTI_EVENT_VM_INIT, ("JVMTI Trg VM init event triggered" ));

  // can now enable events 
  JvmtiEventController::vm_init();

  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_VM_INIT)) {
      EVT_TRACE(JVMTI_EVENT_VM_INIT, ("JVMTI Evt VM init event sent" ));

      JavaThread *thread  = JavaThread::current();
      JvmtiThreadEventMark jem(thread);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventVMInit callback = env->callbacks()->VMInit;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread());
      }
    }
  }  
}


extern "C" {
  typedef void (JNICALL *Agent_OnUnload_t)(JavaVM *);
}

void JvmtiExport::post_vm_death() {
  EVT_TRIG_TRACE(JVMTI_EVENT_VM_DEATH, ("JVMTI Trg VM death event triggered" ));

  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_VM_DEATH)) {
      EVT_TRACE(JVMTI_EVENT_VM_DEATH, ("JVMTI Evt VM death event sent" ));
      
      JavaThread *thread  = JavaThread::current();
      JvmtiEventMark jem(thread);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventVMDeath callback = env->callbacks()->VMDeath;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env());
      }
    }
  }

  JvmtiEnvBase::set_phase(JVMTI_PHASE_DEAD);
  JvmtiEventController::vm_death();

  // Send any Agent_OnUnload notifications
  const char *on_unload_symbols[] = AGENT_ONUNLOAD_SYMBOLS;
  extern struct JavaVM_ main_vm;
  for (AgentLibrary* agent = Arguments::agents(); agent != NULL; agent = agent->next()) {

    // Find the Agent_OnUnload function.
    for (uint symbol_index = 0; symbol_index < ARRAY_SIZE(on_unload_symbols); symbol_index++) {
      Agent_OnUnload_t unload_entry = CAST_TO_FN_PTR(Agent_OnUnload_t,
               hpi::dll_lookup(agent->os_lib(), on_unload_symbols[symbol_index]));

      // Invoke the Agent_OnUnload function
      if (unload_entry != NULL) {
        JavaThread* thread = JavaThread::current();
        ThreadToNativeFromVM ttn(thread);
        HandleMark hm(thread);
        (*unload_entry)(&main_vm);
        break;
      }
    }
  }
}


void JvmtiExport::post_class_load(JavaThread *thread, klassOop klass) {
  HandleMark hm(thread);  
  KlassHandle kh(thread, klass);

  EVT_TRIG_TRACE(JVMTI_EVENT_CLASS_LOAD, ("JVMTI [%s] Trg Class Load triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiThreadState* state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_CLASS_LOAD)) {
      EVT_TRACE(JVMTI_EVENT_CLASS_LOAD, ("JVMTI [%s] Evt Class Load sent %s",  
                                         JvmtiTrace::safe_get_thread_name(thread),
                                         kh()==NULL? "NULL" : Klass::cast(kh())->external_name() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiClassEventMark jem(thread, kh());
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventClassLoad callback = env->callbacks()->ClassLoad;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), jem.jni_class());
      }
    }
  }
}


void JvmtiExport::post_class_prepare(JavaThread *thread, klassOop klass) {
  HandleMark hm(thread);  
  KlassHandle kh(thread, klass);

  EVT_TRIG_TRACE(JVMTI_EVENT_CLASS_PREPARE, ("JVMTI [%s] Trg Class Prepare triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiThreadState* state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_CLASS_PREPARE)) { 
      EVT_TRACE(JVMTI_EVENT_CLASS_PREPARE, ("JVMTI [%s] Evt Class Prepare sent %s",  
                                            JvmtiTrace::safe_get_thread_name(thread),
                                            kh()==NULL? "NULL" : Klass::cast(kh())->external_name() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiClassEventMark jem(thread, kh());
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventClassPrepare callback = env->callbacks()->ClassPrepare;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), jem.jni_class());
      }
    }
  }
}

static int unload_event_lock = 1;

void JvmtiExport::post_class_unload(klassOop klass) {
  Thread *thread = Thread::current();
  HandleMark hm(thread);
  KlassHandle kh(thread, klass);

  EVT_TRIG_TRACE(EXT_EVENT_CLASS_UNLOAD, ("JVMTI [?] Trg Class Unload triggered" ));
  if (JvmtiEventController::is_enabled((jvmtiEvent)EXT_EVENT_CLASS_UNLOAD)) { 
assert(thread->is_VM_thread()||thread->is_GC_task_thread(),"wrong thread");

JavaThread*real_thread=NULL;

    // get JavaThread for whom we are proxy
if(thread->is_VM_thread()){
      real_thread = (JavaThread *)((VMThread *)thread)->vm_operation()->calling_thread();
    } else {
#if 1 // NEEDS_CLEANUP
      // There is no notion of a calling thread for an async PGC operation.
      // So let's piggy back on top of the SurrogateLockerThread to make handles.

assert(UseGenPauselessGC,"Cannot use SLT without PauselessGC");

      // Azul - FIXME 
      // multiple GC task threads that get here cause problems in make_local.
      // Making it single-threaded for now.  Need to change this for Grape.

      while (Atomic::cmpxchg(0, &unload_event_lock, 1) != 1);

      real_thread = (JavaThread*) SurrogateLockerThread::slt();
#endif // 1
    }

    JvmtiEnvIterator it; 
    for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
      if (env->is_enabled((jvmtiEvent)EXT_EVENT_CLASS_UNLOAD)) { 
        EVT_TRACE(EXT_EVENT_CLASS_UNLOAD, ("JVMTI [?] Evt Class Unload sent %s",
                  kh()==NULL? "NULL" : Klass::cast(kh())->external_name() ));

        // do everything manually, since this is a proxy - needs special care
        JNIEnv* jni_env = real_thread->jni_environment();
        jthread jt = (jthread)JNIHandles::make_local(real_thread, real_thread->threadObj());
        jclass jk = (jclass)JNIHandles::make_local(real_thread, Klass::cast(kh())->java_mirror());

        jvmtiExtensionEvent callback = env->ext_callbacks()->ClassUnload;
        if (callback != NULL) {
#if 1 // NEEDS_CLEANUP
#if defined(AZ_PROXIED)
          if (callback == (jvmtiExtensionEvent) proxy_invoke_remote_jvmtiExtensionEvent) {
            // Need to create a pointer type in SlowSignatureIterator for
            // passing JNIEnv*
            // Azul - Fake JNIEnv* as an int or long based on whether the proxy
            // is 32 or 64 bit. The actual callback will treat it as a JNIEnv*.
const char*sig="(IL;L;)";
            if (proxy_is64bit()) {
              sig = "(JL;L;)";
            }
            JNIEnv* proxy_jni_env = (JNIEnv*) JVM_Avm2ProxyJNIEnv(jni_env);
            (*callback)(env->jvmti_external(), EXT_EVENT_CLASS_UNLOAD, sig, proxy_jni_env, jt, jk);
          } else {
            // jvmdi callback
            (*callback)(env->jvmti_external(), jni_env, jt, jk);
          }
#else
          // TODO: Unimplemented() probably isn't the right thing for unproxied builds.
          Unimplemented();
#endif // AZ_PROXIED
#endif // 1
        }

        JNIHandles::destroy_local(jk);
        JNIHandles::destroy_local(jt);
      }
    }

if(thread->is_GC_task_thread()){
      int ret = Atomic::cmpxchg(1, &unload_event_lock, 0);
assert(ret==0,"unload_event_lock compromised?");
    }
  }
}


void JvmtiExport::post_thread_start(JavaThread *thread) {
assert(thread->last_Java_sp(),"must be in vm state");

  EVT_TRIG_TRACE(JVMTI_EVENT_THREAD_START, ("JVMTI [%s] Trg Thread Start event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  // do JVMTI thread initialization (if needed) 
  JvmtiEventController::thread_started(thread);

  // Do not post thread start event for hidden java thread.
  if (JvmtiEventController::is_enabled(JVMTI_EVENT_THREAD_START) &&
      !thread->is_hidden_from_external_view()) {
    JvmtiEnvIterator it; 
    for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
      if (env->is_enabled(JVMTI_EVENT_THREAD_START)) { 
        EVT_TRACE(JVMTI_EVENT_THREAD_START, ("JVMTI [%s] Evt Thread Start event sent",  
                     JvmtiTrace::safe_get_thread_name(thread) ));

        JvmtiThreadEventMark jem(thread);
        JvmtiJavaThreadEventTransition jet(thread);
        jvmtiEventThreadStart callback = env->callbacks()->ThreadStart;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread());
        }
      }
    }
  }
}


void JvmtiExport::post_thread_end(JavaThread *thread) {
  EVT_TRIG_TRACE(JVMTI_EVENT_THREAD_END, ("JVMTI [%s] Trg Thread End event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }

  // Do not post thread end event for hidden java thread.
  if (state->is_enabled(JVMTI_EVENT_THREAD_END) &&
      !thread->is_hidden_from_external_view()) {

    JvmtiEnvThreadStateIterator it(state); 
    for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
      if (ets->is_enabled(JVMTI_EVENT_THREAD_END)) {
        EVT_TRACE(JVMTI_EVENT_THREAD_END, ("JVMTI [%s] Evt Thread End event sent",  
                     JvmtiTrace::safe_get_thread_name(thread) ));

        JvmtiEnv *env = ets->get_env();
        JvmtiThreadEventMark jem(thread);
        JvmtiJavaThreadEventTransition jet(thread);
        jvmtiEventThreadEnd callback = env->callbacks()->ThreadEnd;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread());
        }
      }
    }
  }
}

void JvmtiExport::post_object_free(JvmtiEnv* env, jlong tag) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be executed at safepoint");
  assert(env->is_enabled(JVMTI_EVENT_OBJECT_FREE), "checking");

  EVT_TRIG_TRACE(JVMTI_EVENT_OBJECT_FREE, ("JVMTI [?] Trg Object Free triggered" ));
  EVT_TRACE(JVMTI_EVENT_OBJECT_FREE, ("JVMTI [?] Evt Object Free sent"));

  jvmtiEventObjectFree callback = env->callbacks()->ObjectFree;
  if (callback != NULL) {
    (*callback)(env->jvmti_external(), tag);
  }
}

void JvmtiExport::post_resource_exhausted(jint resource_exhausted_flags, const char* description) {
  EVT_TRIG_TRACE(JVMTI_EVENT_RESOURCE_EXHAUSTED, ("JVMTI Trg resource exhausted event triggered" ));

  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_RESOURCE_EXHAUSTED)) {
      EVT_TRACE(JVMTI_EVENT_RESOURCE_EXHAUSTED, ("JVMTI Evt resource exhausted event sent" ));

      JavaThread *thread  = JavaThread::current();
      JvmtiThreadEventMark jem(thread);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventResourceExhausted callback = env->callbacks()->ResourceExhausted;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), 
                    resource_exhausted_flags, NULL, description);
      }
    }
  }  
}

void JvmtiExport::post_method_entry(JavaThread *thread, methodOop method, frame current_frame) {
  HandleMark hm(thread);  
  methodHandle mh(thread, method);

  EVT_TRIG_TRACE(JVMTI_EVENT_METHOD_ENTRY, ("JVMTI [%s] Trg Method Entry triggered %s.%s",  
                     JvmtiTrace::safe_get_thread_name(thread),
                     (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                     (mh() == NULL) ? "NULL" : mh()->name()->as_C_string() ));

  JvmtiThreadState* state = thread->jvmti_thread_state();
  if (state == NULL || !state->is_interp_only_mode()) {
    // for any thread that actually wants method entry, interp_only_mode is set
    return;
  }

  state->incr_cur_stack_depth();

  if (state->is_enabled(JVMTI_EVENT_METHOD_ENTRY)) {
    JvmtiEnvThreadStateIterator it(state); 
    for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
      if (ets->is_enabled(JVMTI_EVENT_METHOD_ENTRY)) {
        EVT_TRACE(JVMTI_EVENT_METHOD_ENTRY, ("JVMTI [%s] Evt Method Entry sent %s.%s",  
                                             JvmtiTrace::safe_get_thread_name(thread),
                                             (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                                             (mh() == NULL) ? "NULL" : mh()->name()->as_C_string() ));
    
        JvmtiEnv *env = ets->get_env();
        JvmtiMethodEventMark jem(thread, mh);
        JvmtiJavaThreadEventTransition jet(thread);
        jvmtiEventMethodEntry callback = env->callbacks()->MethodEntry;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), jem.jni_methodID());
        }
      }
    }
  }
}

void JvmtiExport::post_method_exit(JavaThread *thread, methodOop method, frame current_frame, uint64_t return_value) {
  HandleMark hm(thread);
  methodHandle mh(thread, method);

  EVT_TRIG_TRACE(JVMTI_EVENT_METHOD_EXIT, ("JVMTI [%s] Trg Method Exit triggered %s.%s",  
                     JvmtiTrace::safe_get_thread_name(thread),
                     (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                     (mh() == NULL) ? "NULL" : mh()->name()->as_C_string() ));

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL || !state->is_interp_only_mode()) {
    // for any thread that actually wants method exit, interp_only_mode is set
    return;
  }

  // return a flag when a method terminates by throwing an exception
  // i.e. if an exception is thrown and it's not caught by the current method
  bool exception_exit = state->is_exception_detected() && !state->is_exception_caught();


  if (state->is_enabled(JVMTI_EVENT_METHOD_EXIT)) {
    Handle result;
    jvalue value;	   
    value.j = 0L;

    // if the method hasn't been popped because of an exception then we populate
    // the return_value parameter for the callback. At this point we only have
    // the address of a "raw result" and we just call into the interpreter to 
    // convert this into a jvalue.
    if (!exception_exit) {
      oop oop_result;
BasicType type=current_frame.interpreter_frame_result(&oop_result,&value,return_value);
      if (type == T_OBJECT || type == T_ARRAY) {
        result = Handle(thread, oop_result);
      }
    }

    JvmtiEnvThreadStateIterator it(state); 
    for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
      if (ets->is_enabled(JVMTI_EVENT_METHOD_EXIT)) {
        EVT_TRACE(JVMTI_EVENT_METHOD_EXIT, ("JVMTI [%s] Evt Method Exit sent %s.%s",  
                                            JvmtiTrace::safe_get_thread_name(thread),
                                            (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                                            (mh() == NULL) ? "NULL" : mh()->name()->as_C_string() ));

        JvmtiEnv *env = ets->get_env();

        JvmtiMethodEventMark jem(thread, mh);
	if (result.not_null()) {
	  value.l = JNIHandles::make_local(thread, result());
	}
   	JvmtiJavaThreadEventTransition jet(thread);
        jvmtiEventMethodExit callback = env->callbacks()->MethodExit;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                      jem.jni_methodID(), exception_exit,  value);
        }
      }
    }
  }

  if (state->is_enabled(JVMTI_EVENT_FRAME_POP)) {
    JvmtiEnvThreadStateIterator it(state); 
    for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
      int cur_frame_number = state->cur_stack_depth();

      if (ets->is_frame_pop(cur_frame_number)) {
        // we have a NotifyFramePop entry for this frame.
        // now check that this env/thread wants this event
        if (ets->is_enabled(JVMTI_EVENT_FRAME_POP)) {
          EVT_TRACE(JVMTI_EVENT_FRAME_POP, ("JVMTI [%s] Evt Frame Pop sent %s.%s",  
                                            JvmtiTrace::safe_get_thread_name(thread),
                                            (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                                            (mh() == NULL) ? "NULL" : mh()->name()->as_C_string() ));

          // we also need to issue a frame pop event for this frame
          JvmtiEnv *env = ets->get_env();
	  JvmtiMethodEventMark jem(thread, mh);
          JvmtiJavaThreadEventTransition jet(thread);
          jvmtiEventFramePop callback = env->callbacks()->FramePop;
          if (callback != NULL) {
            (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                        jem.jni_methodID(), exception_exit);
          }
        }
        // remove the frame's entry
        ets->clear_frame_pop(cur_frame_number);
      }
    }
  }

  state->decr_cur_stack_depth();
}


// Todo: inline this for optimization
void JvmtiExport::post_single_step(JavaThread *thread, methodOop method, address location) {
  HandleMark hm(thread);  
  methodHandle mh(thread, method);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    ets->compare_and_set_current_location(mh(), location, JVMTI_EVENT_SINGLE_STEP);
    if (!ets->single_stepping_posted() && ets->is_enabled(JVMTI_EVENT_SINGLE_STEP)) {
EVT_TRACE(JVMTI_EVENT_SINGLE_STEP,("JVMTI [%s] Evt Single Step sent %s.%s @ %ld",
                    JvmtiTrace::safe_get_thread_name(thread),
                    (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                    (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                    location - mh()->code_base() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiLocationEventMark jem(thread, mh, location);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventSingleStep callback = env->callbacks()->SingleStep;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                    jem.jni_methodID(), jem.location());
      }

      ets->set_single_stepping_posted();
    }
  }
}


void JvmtiExport::post_exception_throw(JavaThread *thread, methodOop method, address location, oop exception) {
  HandleMark hm(thread);  
  methodHandle mh(thread, method);
  Handle exception_handle(thread, exception);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  
  EVT_TRIG_TRACE(JVMTI_EVENT_EXCEPTION, ("JVMTI [%s] Trg Exception thrown triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  if (!state->is_exception_detected()) {
    state->set_exception_detected();
    JvmtiEnvThreadStateIterator it(state); 
    for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
      if (ets->is_enabled(JVMTI_EVENT_EXCEPTION) && (exception != NULL)) {

        EVT_TRACE(JVMTI_EVENT_EXCEPTION,
("JVMTI [%s] Evt Exception thrown sent %s.%s @ %ld",
                      JvmtiTrace::safe_get_thread_name(thread),
                      (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                      (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                      location - mh()->code_base() ));
      
        JvmtiEnv *env = ets->get_env();
        JvmtiExceptionEventMark jem(thread, mh, location, exception_handle);
    
        // It's okay to clear these exceptions here because we duplicate
        // this lookup in InterpreterRuntime::exception_handler_for_exception.
        EXCEPTION_MARK;

        methodOop current_method = NULL;
        int current_bci = InvocationEntryBci;
        {
          vframe vf(thread);
          assert(!vf.done(), "cannot be at end");
          do {
            {
              // Get methodOop before vframeAcrossSafepoint sets _method to badOop
methodHandle mh(thread,vf.method());

current_bci=vf.bci();
              bool should_repeat;
              do {
                should_repeat = false;
                KlassHandle eh_klass(thread, exception_handle()->klass());
current_bci=mh()->fast_exception_handler_bci_for(
                  eh_klass, current_bci, THREAD);
                if (HAS_PENDING_EXCEPTION) {
                  exception_handle = KlassHandle(thread, PENDING_EXCEPTION);
                  CLEAR_PENDING_EXCEPTION;
                  should_repeat = true;
                }
}while(should_repeat&&(current_bci!=InvocationEntryBci));
}
            current_method = vf.method(); // Update current_method after GC points
vf.next();
          } while ((current_bci < 0) && (!vf.done()));
        }

        jmethodID catch_jmethodID;
        if (current_bci < 0) {
          catch_jmethodID = 0;
          current_bci = 0;
        } else {
          catch_jmethodID = jem.to_jmethodID(
                                     methodHandle(thread, current_method));
        }

        JvmtiJavaThreadEventTransition jet(thread);
        jvmtiEventException callback = env->callbacks()->Exception;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                      jem.jni_methodID(), jem.location(),
                      jem.exception(), 
                      catch_jmethodID, current_bci);
        }
      }
    }
  }

  // frames may get popped because of this throw, be safe - invalidate cached depth
  state->invalidate_cur_stack_depth();
}


void JvmtiExport::notice_unwind_due_to_exception(JavaThread *thread, methodOop method, address location, oop exception, bool in_handler_frame) {
  HandleMark hm(thread);  
  methodHandle mh(thread, method);
  Handle exception_handle(thread, exception);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  EVT_TRIG_TRACE(JVMTI_EVENT_EXCEPTION_CATCH, 
("JVMTI [%s] Trg unwind_due_to_exception triggered %s.%s @ %s%ld - %s",
                     JvmtiTrace::safe_get_thread_name(thread),
                     (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                     (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                     location==0? "no location:" : "", 
                     location==0? 0 : location - mh()->code_base(),
                     in_handler_frame? "in handler frame" : "not handler frame" )); 
  
  if (state->is_exception_detected()) {
      
    state->invalidate_cur_stack_depth();
    if (!in_handler_frame) {
      // Not in exception handler. 
      if(state->is_interp_only_mode()) {
        // method exit and frame pop events are posted only in interp mode.
        // When these events are enabled code should be in running in interp mode.
        if (method->is_native()) {
          ShouldNotReachHere();
        }
        JvmtiExport::post_method_exit(thread, method, thread->last_frame(), 0);
        // The cached cur_stack_depth might have changed from the
        // operations of frame pop or method exit. We are not 100% sure
        // the cached cur_stack_depth is still valid depth so invalidate
        // it. 
        state->invalidate_cur_stack_depth();
      }
    } else {
      // In exception handler frame. Report exception catch.
      assert(location != NULL, "must be a known location");
      // Update cur_stack_depth - the frames above the current frame
      // have been unwound due to this exception:
      assert(!state->is_exception_caught(), "exception must not be caught yet.");
      state->set_exception_caught();

      JvmtiEnvThreadStateIterator it(state); 
      for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
        if (ets->is_enabled(JVMTI_EVENT_EXCEPTION_CATCH) && (exception_handle() != NULL)) {
          EVT_TRACE(JVMTI_EVENT_EXCEPTION_CATCH, 
("JVMTI [%s] Evt ExceptionCatch sent %s.%s @ %ld",
                      JvmtiTrace::safe_get_thread_name(thread),
                      (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                      (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                      location - mh()->code_base() )); 

          JvmtiEnv *env = ets->get_env();
          JvmtiExceptionEventMark jem(thread, mh, location, exception_handle);
          JvmtiJavaThreadEventTransition jet(thread);
          jvmtiEventExceptionCatch callback = env->callbacks()->ExceptionCatch;
          if (callback != NULL) {
            (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                      jem.jni_methodID(), jem.location(),
                      jem.exception());
          }
        }
      }
    }
  }
}

oop JvmtiExport::jni_GetField_probe(JavaThread *thread, jobject jobj, oop obj,
                                    klassOop klass, jfieldID fieldID, bool is_static) {
if(*((int*)get_field_access_count_addr())>0)
{//No javaFrameAnchor, so no need to test has_last_Java_frame()
    // At least one field access watch is set so we have more work
    // to do. This wrapper is used by entry points that allow us
    // to create handles in post_field_access_by_jni().
    post_field_access_by_jni(thread, obj, klass, fieldID, is_static);
    // event posting can block so refetch oop if we were passed a jobj
    if (jobj != NULL) return JNIHandles::resolve_non_null(jobj);
  }
  return obj;
}

oop JvmtiExport::jni_GetField_probe_nh(JavaThread *thread, jobject jobj, oop obj,
                                       klassOop klass, jfieldID fieldID, bool is_static) {
if(*((int*)get_field_access_count_addr())>0)
{//No javaFrameAnchor, so no need to test has_last_Java_frame()
    // At least one field access watch is set so we have more work
    // to do. This wrapper is used by "quick" entry points that don't
    // allow us to create handles in post_field_access_by_jni(). We
    // override that with a ResetNoHandleMark.
    ResetNoHandleMark rnhm;
    post_field_access_by_jni(thread, obj, klass, fieldID, is_static);
    // event posting can block so refetch oop if we were passed a jobj
    if (jobj != NULL) return JNIHandles::resolve_non_null(jobj);
  }
  return obj;
}

void JvmtiExport::post_field_access_by_jni(JavaThread *thread, oop obj,
                                           klassOop klass, jfieldID fieldID, bool is_static) {
  // We must be called with a Java context in order to provide reasonable
  // values for the klazz, method, and location fields. The callers of this
  // function don't make the call unless there is a Java context.
assert(thread->last_Java_sp(),"must be called with a Java context");

  ResourceMark rm;
  fieldDescriptor fd;
  // if get_field_descriptor finds fieldID to be invalid, then we just bail
  bool valid_fieldID = JvmtiEnv::get_field_descriptor(klass, fieldID, &fd);
  assert(valid_fieldID == true,"post_field_access_by_jni called with invalid fieldID");
  if (!valid_fieldID) return;
  // field accesses are not watched so bail
  if (!fd.is_field_access_watched()) return;

  HandleMark hm(thread);
  KlassHandle h_klass(thread, klass);
  Handle h_obj;
  if (!is_static) {
    // non-static field accessors have an object, but we need a handle
    assert(obj != NULL, "non-static needs an object");
    h_obj = Handle(thread, obj);
  }

methodOop method=NULL;
int bci=InvocationEntryBci;
  {
    vframe vf = thread->last_java_vframe();
    method = vf.method();
    bci = vf.bci();
  }
  address bcp = method->bcp_from(bci);

  post_field_access(thread, method, bcp, h_klass, h_obj, fieldID);
}

void JvmtiExport::post_field_access(JavaThread *thread, methodOop method,
  address location, KlassHandle field_klass, Handle object, jfieldID field) {

  HandleMark hm(thread);
  methodHandle mh(thread, method);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  EVT_TRIG_TRACE(JVMTI_EVENT_FIELD_ACCESS, ("JVMTI [%s] Trg Field Access event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_FIELD_ACCESS)) {
EVT_TRACE(JVMTI_EVENT_FIELD_ACCESS,("JVMTI [%s] Evt Field Access event sent %s.%s @ %ld",
                     JvmtiTrace::safe_get_thread_name(thread),
                     (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                     (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                     location - mh()->code_base() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiLocationEventMark jem(thread, mh, location);
      jclass field_jclass = jem.to_jclass(field_klass());
      jobject field_jobject = jem.to_jobject(object());
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventFieldAccess callback = env->callbacks()->FieldAccess;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                    jem.jni_methodID(), jem.location(),
                    field_jclass, field_jobject, field);
      }
    }
  }
}

oop JvmtiExport::jni_SetField_probe(JavaThread *thread, jobject jobj, oop obj,
                                    klassOop klass, jfieldID fieldID, bool is_static,
                                    char sig_type, jvalue *value) {
if(*((int*)get_field_modification_count_addr())>0)
{//No javaFrameAnchor, so no need to test has_last_Java_frame()
    // At least one field modification watch is set so we have more work
    // to do. This wrapper is used by entry points that allow us
    // to create handles in post_field_modification_by_jni().
    post_field_modification_by_jni(thread, obj, klass, fieldID, is_static, sig_type, value);
    // event posting can block so refetch oop if we were passed a jobj
    if (jobj != NULL) return JNIHandles::resolve_non_null(jobj);
  }
  return obj;
}

oop JvmtiExport::jni_SetField_probe_nh(JavaThread *thread, jobject jobj, oop obj,
                                       klassOop klass, jfieldID fieldID, bool is_static,
                                       char sig_type, jvalue *value) {
if(*((int*)get_field_modification_count_addr())>0)
{//No javaFrameAnchor, so no need to test has_last_Java_frame()
    // At least one field modification watch is set so we have more work
    // to do. This wrapper is used by "quick" entry points that don't
    // allow us to create handles in post_field_modification_by_jni(). We
    // override that with a ResetNoHandleMark.
    ResetNoHandleMark rnhm;
    post_field_modification_by_jni(thread, obj, klass, fieldID, is_static, sig_type, value);
    // event posting can block so refetch oop if we were passed a jobj
    if (jobj != NULL) return JNIHandles::resolve_non_null(jobj);
  }
  return obj;
}

void JvmtiExport::post_field_modification_by_jni(JavaThread *thread, oop obj,
                                                 klassOop klass, jfieldID fieldID, bool is_static,
                                                 char sig_type, jvalue *value) {
  // We must be called with a Java context in order to provide reasonable
  // values for the klazz, method, and location fields. The callers of this
  // function don't make the call unless there is a Java context.
assert(thread->last_Java_sp(),"must be called with Java context");

  ResourceMark rm;
  fieldDescriptor fd;
  // if get_field_descriptor finds fieldID to be invalid, then we just bail
  bool valid_fieldID = JvmtiEnv::get_field_descriptor(klass, fieldID, &fd);
  assert(valid_fieldID == true,"post_field_modification_by_jni called with invalid fieldID");
  if (!valid_fieldID) return;
  // field modifications are not watched so bail
  if (!fd.is_field_modification_watched()) return;

  HandleMark hm(thread);

  Handle h_obj;
  if (!is_static) {
    // non-static field accessors have an object, but we need a handle
    assert(obj != NULL, "non-static needs an object");
    h_obj = Handle(thread, obj);
  }
  KlassHandle h_klass(thread, klass);

methodOop method=NULL;
int bci=InvocationEntryBci;
  {
    vframe vf = thread->last_java_vframe();
    method = vf.method();
    bci = vf.bci();
  }
  address bcp = method->bcp_from(bci);

post_field_modification(thread,method,bcp,h_klass,h_obj,fieldID,sig_type,value);
}

void JvmtiExport::post_raw_field_modification(JavaThread *thread, methodOop method,
  address location, KlassHandle field_klass, Handle object, jfieldID field,
  char sig_type, jvalue *value) {

  if (sig_type == 'I' || sig_type == 'Z' || sig_type == 'C' || sig_type == 'S') {
    // 'I' instructions are used for byte, char, short and int.
    // determine which it really is, and convert
    fieldDescriptor fd;
    bool found = JvmtiEnv::get_field_descriptor(field_klass(), field, &fd);
    // should be found (if not, leave as is)
    if (found) {
      switch (fd.field_type()) {   
case T_BOOLEAN:sig_type='Z';break;
case T_BYTE:sig_type='B';break;
case T_CHAR:sig_type='C';break;
case T_SHORT:sig_type='S';break;
case T_INT:sig_type='I';break;
      default:
        // this is an integer instruction, should be one of above
        ShouldNotReachHere();
        break;
      }
    }
  }

  // convert oop to JNI handle.
  if (sig_type == 'L' || sig_type == '[') {
value->l=(jobject)JNIHandles::make_local(thread,objectRef((uint64_t)value->l).as_oop());
  }

  post_field_modification(thread, method, location, field_klass, object, field, sig_type, value);

  // Destroy the JNI handle allocated above.
  if (sig_type == 'L') {
    JNIHandles::destroy_local(value->l);
  }
}

void JvmtiExport::post_field_modification(JavaThread *thread, methodOop method,
  address location, KlassHandle field_klass, Handle object, jfieldID field,
  char sig_type, jvalue *value_ptr) {

  HandleMark hm(thread);
  methodHandle mh(thread, method);

  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }
  EVT_TRIG_TRACE(JVMTI_EVENT_FIELD_MODIFICATION, 
                     ("JVMTI [%s] Trg Field Modification event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_FIELD_MODIFICATION)) {
      EVT_TRACE(JVMTI_EVENT_FIELD_MODIFICATION,
("JVMTI [%s] Evt Field Modification event sent %s.%s @ %ld",
                    JvmtiTrace::safe_get_thread_name(thread),
                    (mh() == NULL) ? "NULL" : mh()->klass_name()->as_C_string(),
                    (mh() == NULL) ? "NULL" : mh()->name()->as_C_string(),
                    location - mh()->code_base() ));

      JvmtiEnv *env = ets->get_env();
      JvmtiLocationEventMark jem(thread, mh, location);
      jclass field_jclass = jem.to_jclass(field_klass());
      jobject field_jobject = jem.to_jobject(object());
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventFieldModification callback = env->callbacks()->FieldModification;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                    jem.jni_methodID(), jem.location(),
                    field_jclass, field_jobject, field, sig_type, *value_ptr);
      }
    }
  }
}

class JvmtiClassFileLoadHookPoster : public StackObj {
 private:
  symbolHandle         _h_name;
  Handle               _class_loader;
  Handle               _h_protection_domain;
  unsigned char **     _data_ptr;
  unsigned char **     _end_ptr;
  JavaThread *         _thread;
  jint                 _curr_len;
  unsigned char *      _curr_data;
  JvmtiEnv *           _curr_env; 
  jint *               _cached_length_ptr;
  unsigned char **     _cached_data_ptr;
  JvmtiThreadState *   _state;
  KlassHandle *        _h_class_being_redefined;
  JvmtiClassLoadKind   _load_kind;

 public:
  inline JvmtiClassFileLoadHookPoster(symbolHandle h_name, Handle class_loader,
                                      Handle h_protection_domain, 
                                      unsigned char **data_ptr, unsigned char **end_ptr, 
                                      unsigned char **cached_data_ptr, 
                                      jint *cached_length_ptr) {
    _h_name = h_name;
    _class_loader = class_loader;
    _h_protection_domain = h_protection_domain;
    _data_ptr = data_ptr;
    _end_ptr = end_ptr;
    _thread = JavaThread::current();
    _curr_len = *end_ptr - *data_ptr;
    _curr_data = *data_ptr;
    _curr_env = NULL; 
    _cached_length_ptr = cached_length_ptr;
    _cached_data_ptr = cached_data_ptr;
    *_cached_length_ptr = 0;
    *_cached_data_ptr = NULL;

    _state = _thread->jvmti_thread_state();
    if (_state != NULL) {
      _h_class_being_redefined = _state->get_class_being_redefined();
      _load_kind = _state->get_class_load_kind();
      // Clear class_being_redefined flag here. The action 
      // from agent handler could generate a new class file load
      // hook event and if it is not cleared the new event generated
      // from regular class file load could have this stale redefined
      // class handle info. 
      _state->clear_class_being_redefined();
    } else {
      // redefine and retransform will always set the thread state
      _h_class_being_redefined = (KlassHandle *) NULL;
      _load_kind = jvmti_class_load_kind_load;
    }
  }

  void post() {
//    EVT_TRIG_TRACE(JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,
//                   ("JVMTI [%s] class file load hook event triggered",  
//                    JvmtiTrace::safe_get_thread_name(_thread)));
    post_all_envs();
    copy_modified_data();
  }

 private:
  void post_all_envs() {
    if (_load_kind != jvmti_class_load_kind_retransform) {
      // for class load and redefine, 
      // call the non-retransformable agents
      JvmtiEnvIterator it; 
      for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
        if (!env->is_retransformable() && env->is_enabled(JVMTI_EVENT_CLASS_FILE_LOAD_HOOK)) {
          // non-retransformable agents cannot retransform back,
          // so no need to cache the original class file bytes
          post_to_env(env, false);
        }
      }
    }
    JvmtiEnvIterator it; 
    for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
      // retransformable agents get all events
      if (env->is_retransformable() && env->is_enabled(JVMTI_EVENT_CLASS_FILE_LOAD_HOOK)) {
        // retransformable agents need to cache the original class file 
        // bytes if changes are made via the ClassFileLoadHook
        post_to_env(env, true);
      }
    }
  }

  void post_to_env(JvmtiEnv* env, bool caching_needed) {
    unsigned char *new_data = NULL;
    jint new_len = 0;
//    EVT_TRACE(JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,
//     ("JVMTI [%s] class file load hook event sent %s  data_ptr = %d, data_len = %d",  
//               JvmtiTrace::safe_get_thread_name(_thread),
//               _h_name.is_null() ? "NULL" : _h_name->as_utf8(),
//               _curr_data, _curr_len ));
    JvmtiClassFileLoadEventMark jem(_thread, _h_name, _class_loader, 
                                    _h_protection_domain, 
                                    _h_class_being_redefined);
    JvmtiJavaThreadEventTransition jet(_thread);
    JNIEnv* jni_env =  (JvmtiEnv::get_phase() == JVMTI_PHASE_PRIMORDIAL)? 
                                                        NULL : jem.jni_env();
    jvmtiEventClassFileLoadHook callback = env->callbacks()->ClassFileLoadHook;
    if (callback != NULL) {
      (*callback)(env->jvmti_external(), jni_env,
                  jem.class_being_redefined(),
                  jem.jloader(), jem.class_name(), 
                  jem.protection_domain(),
                  _curr_len, _curr_data,
                  &new_len, &new_data);
    }
    if (new_data != NULL) {
      // this agent has modified class data. 
      if (caching_needed && *_cached_data_ptr == NULL) {
        // data has been changed by the new retransformable agent
        // and it hasn't already been cached, cache it
        *_cached_data_ptr = (unsigned char *)os::malloc(_curr_len);
        memcpy(*_cached_data_ptr, _curr_data, _curr_len);
        *_cached_length_ptr = _curr_len;
      }

      if (_curr_data != *_data_ptr) {
        // curr_data is previous agent modified class data.
        // And this has been changed by the new agent so
        // we can delete it now.           
        _curr_env->Deallocate(_curr_data);
      }

      // Class file data has changed by the current agent.
      _curr_data = new_data;
      _curr_len = new_len;
      // Save the current agent env we need this to deallocate the
      // memory allocated by this agent.
      _curr_env = env;
    }
  }

  void copy_modified_data() {
    // if one of the agent has modified class file data.
    // Copy modified class data to new resources array.
    if (_curr_data != *_data_ptr) {
      *_data_ptr = NEW_RESOURCE_ARRAY(u1, _curr_len);
      memcpy(*_data_ptr, _curr_data, _curr_len);
      *_end_ptr = *_data_ptr + _curr_len;
      _curr_env->Deallocate(_curr_data);
    }
  }
};

// this entry is for class file load hook on class load, redefine and retransform
void JvmtiExport::post_class_file_load_hook(symbolHandle h_name, 
                                            Handle class_loader,
                                            Handle h_protection_domain, 
                                            unsigned char **data_ptr, 
                                            unsigned char **end_ptr, 
                                            unsigned char **cached_data_ptr, 
                                            jint *cached_length_ptr) {
  JvmtiClassFileLoadHookPoster poster(h_name, class_loader, 
                                      h_protection_domain, 
                                      data_ptr, end_ptr,
                                      cached_data_ptr, 
                                      cached_length_ptr);
  poster.post();
}

void JvmtiExport::post_native_method_bind(methodOop method, address* function_ptr) {
  JavaThread* thread = JavaThread::current();    

  HandleMark hm(thread);
  methodHandle mh(thread, method);

  EVT_TRIG_TRACE(JVMTI_EVENT_NATIVE_METHOD_BIND, ("JVMTI [%s] Trg Native Method Bind event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  if (JvmtiEventController::is_enabled(JVMTI_EVENT_NATIVE_METHOD_BIND)) {
    JvmtiEnvIterator it; 
    for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
      if (env->is_enabled(JVMTI_EVENT_NATIVE_METHOD_BIND)) { 
        EVT_TRACE(JVMTI_EVENT_NATIVE_METHOD_BIND, ("JVMTI [%s] Evt Native Method Bind event sent",  
                     JvmtiTrace::safe_get_thread_name(thread) ));

        JvmtiMethodEventMark jem(thread, mh);
        JvmtiJavaThreadEventTransition jet(thread);
        JNIEnv* jni_env =  JvmtiEnv::get_phase() == JVMTI_PHASE_PRIMORDIAL? NULL : jem.jni_env();
        jvmtiEventNativeMethodBind callback = env->callbacks()->NativeMethodBind;
        if (callback != NULL) {
          (*callback)(env->jvmti_external(), jni_env, jem.jni_thread(), 
                      jem.jni_methodID(), (void*)(*function_ptr), (void**)function_ptr);
        }
      }
    }
  }
}


void JvmtiExport::post_compiled_method_load(methodCodeOop nm) {
  // If there are pending CompiledMethodUnload events then these are 
  // posted before this CompiledMethodLoad event.  We "lock" the methodCode and
  // maintain a handle to ensure that the methodCode isn't
  // flushed or unloaded while posting the events.
  methodCodeHandle nmch(nm);
  JavaThread* thread = JavaThread::current();    
  if (have_pending_compiled_method_unload_events()) {
    post_pending_compiled_method_unload_events();
  }

  EVT_TRIG_TRACE(JVMTI_EVENT_COMPILED_METHOD_LOAD,
                 ("JVMTI [%s] method compile load event triggered",  
                 JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_COMPILED_METHOD_LOAD)) {

      EVT_TRACE(JVMTI_EVENT_COMPILED_METHOD_LOAD,
                ("JVMTI [%s] class compile method load event sent %s.%s  ",
                JvmtiTrace::safe_get_thread_name(thread),
                 (nmch()->method().is_null()) ? "NULL" : nmch()->method().as_methodOop()->klass_name()->as_C_string(),
                 (nmch()->method().is_null()) ? "NULL" : nmch()->method().as_methodOop()->name()->as_C_string()));

      ResourceMark rm(thread);  
JvmtiCompiledMethodLoadEventMark jem(thread,&nmch);
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventCompiledMethodLoad callback = env->callbacks()->CompiledMethodLoad;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_methodID(),
                    jem.code_size(), jem.code_data(), jem.map_length(),
                    jem.map(), jem.compile_info());
      }
    }
  }
}


// post a COMPILED_METHOD_LOAD event for a given environment
void JvmtiExport::post_compiled_method_load(JvmtiEnv* env, const jmethodID method, const jint length, 
					    const void *code_begin, const jint map_length, 
					    const jvmtiAddrLocationMap* map)
{
  JavaThread* thread = JavaThread::current();    
  EVT_TRIG_TRACE(JVMTI_EVENT_COMPILED_METHOD_LOAD,
                 ("JVMTI [%s] method compile load event triggered (by GenerateEvents)",  
                 JvmtiTrace::safe_get_thread_name(thread))); 
  if (env->is_enabled(JVMTI_EVENT_COMPILED_METHOD_LOAD)) {

    EVT_TRACE(JVMTI_EVENT_COMPILED_METHOD_LOAD,
              ("JVMTI [%s] class compile method load event sent (by GenerateEvents), jmethodID=" PTR_FORMAT,
              JvmtiTrace::safe_get_thread_name(thread), method));

    JvmtiEventMark jem(thread);
    JvmtiJavaThreadEventTransition jet(thread);
    jvmtiEventCompiledMethodLoad callback = env->callbacks()->CompiledMethodLoad;
    if (callback != NULL) {
      (*callback)(env->jvmti_external(), method,
                  length, code_begin, map_length,
                  map, NULL);
    }
  }
}

// used at a safepoint to post a CompiledMethodUnload event
void JvmtiExport::post_compiled_method_unload_at_safepoint(jmethodID mid, const void *code_begin) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be executed at a safepoint");

  // create list lazily
  if (_pending_compiled_method_unload_method_ids == NULL) {
    _pending_compiled_method_unload_method_ids = new (ResourceObj::C_HEAP) GrowableArray<jmethodID>(10,true);
    _pending_compiled_method_unload_code_begins = new (ResourceObj::C_HEAP) GrowableArray<const void *>(10,true);
  }
  _pending_compiled_method_unload_method_ids->append(mid);
  _pending_compiled_method_unload_code_begins->append(code_begin);
  _have_pending_compiled_method_unload_events = true;
}

void JvmtiExport::post_dynamic_code_generated_internal(const char *name, const void *code_begin, const void *code_end) {
  JavaThread* thread = JavaThread::current();
  EVT_TRIG_TRACE(JVMTI_EVENT_DYNAMIC_CODE_GENERATED,
                 ("JVMTI [%s] method dynamic code generated event triggered",
                 JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_DYNAMIC_CODE_GENERATED)) {
      EVT_TRACE(JVMTI_EVENT_DYNAMIC_CODE_GENERATED,
		("JVMTI [%s] dynamic code generated event sent for %s",
		JvmtiTrace::safe_get_thread_name(thread), name));
      JvmtiEventMark jem(thread);
      JvmtiJavaThreadEventTransition jet(thread);
      jint length = (jint)pointer_delta(code_end, code_begin, sizeof(char));
      jvmtiEventDynamicCodeGenerated callback = env->callbacks()->DynamicCodeGenerated;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), name, (void*)code_begin, length);
      }
    }
  }
} 

void JvmtiExport::post_dynamic_code_generated(const char *name, const void *code_begin, const void *code_end) {
  // In theory everyone coming thru here is in_vm but we need to be certain
  // because a callee will do a vm->native transition
  ThreadInVMfromUnknown __tiv;   
  jvmtiPhase phase = JvmtiEnv::get_phase();
  if (phase == JVMTI_PHASE_PRIMORDIAL || phase == JVMTI_PHASE_START) {
    post_dynamic_code_generated_internal(name, code_begin, code_end);
    return;
  }

  if (have_pending_compiled_method_unload_events()) {
    post_pending_compiled_method_unload_events();
  }
  post_dynamic_code_generated_internal(name, code_begin, code_end);
}


// post a DYNAMIC_CODE_GENERATED event for a given environment
// used by GenerateEvents
void JvmtiExport::post_dynamic_code_generated(JvmtiEnv* env, const char *name, 
					      const void *code_begin, const void *code_end)
{
  JavaThread* thread = JavaThread::current();
  EVT_TRIG_TRACE(JVMTI_EVENT_DYNAMIC_CODE_GENERATED,
                 ("JVMTI [%s] dynamic code generated event triggered (by GenerateEvents)",
		  JvmtiTrace::safe_get_thread_name(thread)));
  if (env->is_enabled(JVMTI_EVENT_DYNAMIC_CODE_GENERATED)) {
    EVT_TRACE(JVMTI_EVENT_DYNAMIC_CODE_GENERATED,
              ("JVMTI [%s] dynamic code generated event sent for %s",
	       JvmtiTrace::safe_get_thread_name(thread), name));
    JvmtiEventMark jem(thread);
    JvmtiJavaThreadEventTransition jet(thread);
    jint length = (jint)pointer_delta(code_end, code_begin, sizeof(char));
    jvmtiEventDynamicCodeGenerated callback = env->callbacks()->DynamicCodeGenerated;
    if (callback != NULL) {
      (*callback)(env->jvmti_external(), name, (void*)code_begin, length);
    }
  }
} 

// post a DynamicCodeGenerated event while holding locks in the VM.
void JvmtiExport::post_dynamic_code_generated_while_holding_locks(const char* name, 
								  address code_begin, address code_end)
{
  // register the stub with the current dynamic code event collector
  JvmtiThreadState* state = JvmtiThreadState::state_for(JavaThread::current());
  JvmtiDynamicCodeEventCollector* collector = state->get_dynamic_code_event_collector();
  guarantee(collector != NULL, "attempt to register stub without event collector");
  collector->register_stub(name, code_begin, code_end);
}

// Collect all the vm internally allocated objects which are visible to java world
void JvmtiExport::record_vm_internal_object_allocation(oop obj) {
  Thread* thread = Thread::current();
  if (thread != NULL && thread->is_Java_thread())  {
    // Can not take safepoint here.
    No_Safepoint_Verifier no_sfpt;
    // Can not take safepoint here so can not use state_for to get
    // jvmti thread state.
    JvmtiThreadState *state = ((JavaThread*)thread)->jvmti_thread_state();
    if (state != NULL ) {
      // state is non NULL when VMObjectAllocEventCollector is enabled.
      JvmtiVMObjectAllocEventCollector *collector;
      collector = state->get_vm_object_alloc_event_collector();
      if (collector != NULL && collector->is_enabled()) {
        // Don't record classes as these will be notified via the ClassLoad
        // event.
        if (obj->klass() != SystemDictionary::class_klass()) {	
          collector->record_allocation(obj);
        }
      } 
    }
  }
}

void JvmtiExport::post_garbage_collection_finish() {
  Thread *thread = Thread::current(); // this event is posted from VM-Thread.
  EVT_TRIG_TRACE(JVMTI_EVENT_GARBAGE_COLLECTION_FINISH,
                 ("JVMTI [%s] garbage collection finish event triggered",
                  JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_GARBAGE_COLLECTION_FINISH)) {
      EVT_TRACE(JVMTI_EVENT_GARBAGE_COLLECTION_FINISH,
                ("JVMTI [%s] garbage collection finish event sent ",
                 JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiThreadEventTransition jet(thread);
      // JNIEnv is NULL here because this event is posted from VM Thread
      jvmtiEventGarbageCollectionFinish callback = env->callbacks()->GarbageCollectionFinish;
      if (callback != NULL) {
        (*callback)(env->jvmti_external());
      }
    }
  }
} 

void JvmtiExport::post_garbage_collection_start() {
  Thread* thread = Thread::current(); // this event is posted from vm-thread.
  EVT_TRIG_TRACE(JVMTI_EVENT_GARBAGE_COLLECTION_START,
                 ("JVMTI [%s] garbage collection start event triggered",
                  JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_GARBAGE_COLLECTION_START)) {
      EVT_TRACE(JVMTI_EVENT_GARBAGE_COLLECTION_START,
                ("JVMTI [%s] garbage collection start event sent ",
                 JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiThreadEventTransition jet(thread);
      // JNIEnv is NULL here because this event is posted from VM Thread
      jvmtiEventGarbageCollectionStart callback = env->callbacks()->GarbageCollectionStart;
      if (callback != NULL) {
        (*callback)(env->jvmti_external());
      }
    }
  }
} 

void JvmtiExport::post_data_dump() {
  Thread *thread = Thread::current();
  EVT_TRIG_TRACE(JVMTI_EVENT_DATA_DUMP_REQUEST, 
                 ("JVMTI [%s] data dump request event triggered",
                  JvmtiTrace::safe_get_thread_name(thread)));
  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_DATA_DUMP_REQUEST)) {
      EVT_TRACE(JVMTI_EVENT_DATA_DUMP_REQUEST,
                ("JVMTI [%s] data dump request event sent ",
                 JvmtiTrace::safe_get_thread_name(thread)));
     JvmtiThreadEventTransition jet(thread);
     // JNIEnv is NULL here because this event is posted from VM Thread
     jvmtiEventDataDumpRequest callback = env->callbacks()->DataDumpRequest;
     if (callback != NULL) {
       (*callback)(env->jvmti_external());
     }
    }
  }
}

void JvmtiExport::post_monitor_contended_enter(JavaThread *thread, ObjectMonitor *obj_mntr) {
  oop object = (oop)obj_mntr->object();
  if (!ServiceUtil::visible_oop(object)) {
    // Ignore monitor contended enter for vm internal object. 
    return;
  }
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }

  HandleMark hm(thread);
  Handle h(thread, object);

  EVT_TRIG_TRACE(JVMTI_EVENT_MONITOR_CONTENDED_ENTER, 
                     ("JVMTI [%s] montior contended enter event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_MONITOR_CONTENDED_ENTER)) {
      EVT_TRACE(JVMTI_EVENT_MONITOR_CONTENDED_ENTER,
                   ("JVMTI [%s] monitor contended enter event sent",  
                    JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiMonitorEventMark  jem(thread, h());
      JvmtiEnv *env = ets->get_env();
      JvmtiThreadEventTransition jet(thread);
      jvmtiEventMonitorContendedEnter callback = env->callbacks()->MonitorContendedEnter;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), jem.jni_object());
      }
    }
  }
}

void JvmtiExport::post_monitor_contended_entered(JavaThread *thread, ObjectMonitor *obj_mntr) {
  oop object = (oop)obj_mntr->object();
  if (!ServiceUtil::visible_oop(object)) {
    // Ignore monitor contended entered for vm internal object. 
    return;
  }
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }

  HandleMark hm(thread);
  Handle h(thread, object);

  EVT_TRIG_TRACE(JVMTI_EVENT_MONITOR_CONTENDED_ENTERED, 
                     ("JVMTI [%s] montior contended entered event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_MONITOR_CONTENDED_ENTERED)) {
      EVT_TRACE(JVMTI_EVENT_MONITOR_CONTENDED_ENTERED,
                   ("JVMTI [%s] monitor contended enter event sent",  
                    JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiMonitorEventMark  jem(thread, h());        
      JvmtiEnv *env = ets->get_env();
      JvmtiThreadEventTransition jet(thread);
      jvmtiEventMonitorContendedEntered callback = env->callbacks()->MonitorContendedEntered;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), jem.jni_object());
      }
    }
  }
}

void JvmtiExport::post_monitor_wait(JavaThread *thread, oop object,
                                          jlong timeout) {
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }

  HandleMark hm(thread);
  Handle h(thread, object);
  
  EVT_TRIG_TRACE(JVMTI_EVENT_MONITOR_WAIT, 
                     ("JVMTI [%s] montior wait event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_MONITOR_WAIT)) {
      EVT_TRACE(JVMTI_EVENT_MONITOR_WAIT,
                   ("JVMTI [%s] monitor wait event sent ",  
                    JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiMonitorEventMark  jem(thread, h());
      JvmtiEnv *env = ets->get_env();
      JvmtiThreadEventTransition jet(thread);
      jvmtiEventMonitorWait callback = env->callbacks()->MonitorWait;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(),
                    jem.jni_object(), timeout);
      }
    }
  }
}

void JvmtiExport::post_monitor_waited(JavaThread *thread, ObjectMonitor *obj_mntr, jboolean timed_out) {
  oop object = (oop)obj_mntr->object();
  if (!ServiceUtil::visible_oop(object)) {
    // Ignore monitor waited for vm internal object. 
    return;
  }
  JvmtiThreadState *state = thread->jvmti_thread_state();
  if (state == NULL) {
    return;
  }

  HandleMark hm(thread);
  Handle h(thread, object);

  EVT_TRIG_TRACE(JVMTI_EVENT_MONITOR_WAITED, 
                     ("JVMTI [%s] montior waited event triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));

  JvmtiEnvThreadStateIterator it(state); 
  for (JvmtiEnvThreadState* ets = it.first(); ets != NULL; ets = it.next(ets)) {
    if (ets->is_enabled(JVMTI_EVENT_MONITOR_WAITED)) {
      EVT_TRACE(JVMTI_EVENT_MONITOR_WAITED,
                   ("JVMTI [%s] monitor waited event sent ",  
                    JvmtiTrace::safe_get_thread_name(thread)));
      JvmtiMonitorEventMark  jem(thread, h());
      JvmtiEnv *env = ets->get_env();
      JvmtiThreadEventTransition jet(thread);
      jvmtiEventMonitorWaited callback = env->callbacks()->MonitorWaited;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(),
                    jem.jni_object(), timed_out);
      }
    }
  }
}


void JvmtiExport::post_vm_object_alloc(JavaThread *thread,  oop object) {
  EVT_TRIG_TRACE(JVMTI_EVENT_VM_OBJECT_ALLOC, ("JVMTI [%s] Trg vm object alloc triggered",  
                      JvmtiTrace::safe_get_thread_name(thread)));
  if (object == NULL) {
    return;
  }
  HandleMark hm(thread);
  Handle h(thread, object);
  JvmtiEnvIterator it; 
  for (JvmtiEnv* env = it.first(); env != NULL; env = it.next(env)) {
    if (env->is_enabled(JVMTI_EVENT_VM_OBJECT_ALLOC)) {
      EVT_TRACE(JVMTI_EVENT_VM_OBJECT_ALLOC, ("JVMTI [%s] Evt vmobject alloc sent %s",  
                                         JvmtiTrace::safe_get_thread_name(thread),
                                         object==NULL? "NULL" : Klass::cast(java_lang_Class::as_klassOop(object))->external_name()));
 
      JvmtiVMObjectAllocEventMark jem(thread, h());
      JvmtiJavaThreadEventTransition jet(thread);
      jvmtiEventVMObjectAlloc callback = env->callbacks()->VMObjectAlloc;
      if (callback != NULL) {
        (*callback)(env->jvmti_external(), jem.jni_env(), jem.jni_thread(), 
                    jem.jni_jobject(), jem.jni_class(), jem.size());
      }
    }
  }
}


char**
JvmtiExport::get_all_native_method_prefixes(int* count_ptr) {
  // Have to grab JVMTI thread state lock to be sure environment doesn't
  // go away while we iterate them.  No locks during VM bring-up.
  if (Threads::number_of_threads() == 0 || SafepointSynchronize::is_at_safepoint()) {
    return JvmtiEnvBase::get_all_native_method_prefixes(count_ptr);
  } else {
    MutexLockerAllowGC mu(*JvmtiThreadState_lock, JavaThread::current());
    return JvmtiEnvBase::get_all_native_method_prefixes(count_ptr);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////

void JvmtiExport::cleanup_thread(JavaThread* thread) {
  assert(JavaThread::current() == thread, "thread is not current");


  // This has to happen after the thread state is removed, which is
  // why it is not in post_thread_end_event like its complement
  // Maybe both these functions should be rolled into the posts?
  JvmtiEventController::thread_ended(thread);
}

void JvmtiExport::oops_do(OopClosure* f) {
  JvmtiCurrentBreakpoints::oops_do(f);
  JvmtiVMObjectAllocEventCollector::oops_do_for_all_threads(f);
}

// Onload raw monitor transition.
void JvmtiExport::transition_pending_onload_raw_monitors() {
  JvmtiPendingMonitors::transition_raw_monitors();
}

////////////////////////////////////////////////////////////////////////////////////////////////

// type for the Agent_OnAttach entry point
extern "C" {
  typedef jint (JNICALL *OnAttachEntry_t)(JavaVM*, char *, void *);
}

jint JvmtiExport::load_agent_library(AttachOperation* op, outputStream* st) {
  char ebuf[1024];
  char buffer[JVM_MAXPATHLEN];
  void* library;
  jint result = JNI_ERR;

  // get agent name and options
  const char* agent = op->arg(0);
  const char* absParam = op->arg(1);
  const char* options = op->arg(2);

  // The abs paramter should be "true" or "false"
  bool is_absolute_path = (absParam != NULL) && (strcmp(absParam,"true")==0);


  // If the path is absolute we attempt to load the library. Otherwise we try to 
  // load it from the standard dll directory.

  if (is_absolute_path) {
    library = hpi::dll_load(agent, ebuf, sizeof ebuf);
  } else {
    // Try to load the agent from the standard dll directory
hpi::dll_build_name(buffer,sizeof(buffer),Arguments::get_dll_dir(),agent,UseDebugLibrarySuffix);
    library = hpi::dll_load(buffer, ebuf, sizeof ebuf);
    if (library == NULL) {
      // not found - try local path
      char ns[1] = {0};
hpi::dll_build_name(buffer,sizeof(buffer),ns,agent,UseDebugLibrarySuffix);
      library = hpi::dll_load(buffer, ebuf, sizeof ebuf);
    }
  }

  // If the library was loaded then we attempt to invoke the Agent_OnAttach
  // function
  if (library != NULL) {

    // Lookup the Agent_OnAttach function
    OnAttachEntry_t on_attach_entry = NULL;
    const char *on_attach_symbols[] = AGENT_ONATTACH_SYMBOLS;
    for (uint symbol_index = 0; symbol_index < ARRAY_SIZE(on_attach_symbols); symbol_index++) {
      on_attach_entry = 
	CAST_TO_FN_PTR(OnAttachEntry_t, hpi::dll_lookup(library, on_attach_symbols[symbol_index]));
      if (on_attach_entry != NULL) break;
    }
                                                                                                      
    if (on_attach_entry == NULL) {
      // Agent_OnAttach missing - unload library
      hpi::dll_unload(library);
    } else {
      // Invoke the Agent_OnAttach function 
      JavaThread* THREAD = JavaThread::current();
      {
        extern struct JavaVM_ main_vm;
        JvmtiThreadEventMark jem(THREAD);
        JvmtiJavaThreadEventTransition jet(THREAD);

        result = (*on_attach_entry)(&main_vm, (char*)options, NULL);
      }

      // Agent_OnAttach may have used JNI
      if (HAS_PENDING_EXCEPTION) {
        CLEAR_PENDING_EXCEPTION;
      }

      // If OnAttach returns JNI_OK then we add it to the list of
      // agent libraries so that we can call Agent_OnUnload later.
      if (result == JNI_OK) {
        Arguments::add_loaded_agent(agent, (char*)options, is_absolute_path, library);
      }

      // Agent_OnAttach executed so completion status is JNI_OK
      st->print_cr("%d", result);
      result = JNI_OK;
    }
  }
  return result;
}

// GPGC has completed referencing processing so may need to update
// tag maps. 
void JvmtiExport::gpgc_ref_processing_epilogue(){
  if (JvmtiEnv::environments_might_exist()) {
JvmtiTagMap::gpgc_ref_processing_epilogue();
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////

// Setup current current thread for event collection.
void JvmtiEventCollector::setup_jvmti_thread_state() {
  // set this event collector to be the current one.
  JvmtiThreadState* state = JvmtiThreadState::state_for(JavaThread::current());
  if (is_vm_object_alloc_event()) { 
    _prev = state->get_vm_object_alloc_event_collector(); 
    state->set_vm_object_alloc_event_collector((JvmtiVMObjectAllocEventCollector *)this);
  } else if (is_dynamic_code_event()) {
    _prev = state->get_dynamic_code_event_collector(); 
    state->set_dynamic_code_event_collector((JvmtiDynamicCodeEventCollector *)this);
  }
}

// Unset current event collection in this thread and reset it with previous
// collector.
void JvmtiEventCollector::unset_jvmti_thread_state() {
  JvmtiThreadState* state = JavaThread::current()->jvmti_thread_state();
  if (state != NULL) {
    // restore the previous event collector (if any)    
    if (is_vm_object_alloc_event()) { 
      if (state->get_vm_object_alloc_event_collector() == this) {
        state->set_vm_object_alloc_event_collector((JvmtiVMObjectAllocEventCollector *)_prev);
      } else {
	// this thread's jvmti state was created during the scope of
	// the event collector.
      }
    } else {
      if (is_dynamic_code_event()) {
	if (state->get_dynamic_code_event_collector() == this) {
          state->set_dynamic_code_event_collector((JvmtiDynamicCodeEventCollector *)_prev);
	} else {
	  // this thread's jvmti state was created during the scope of
	  // the event collector.
	}
      }
    }
  }
}

// create the dynamic code event collector
JvmtiDynamicCodeEventCollector::JvmtiDynamicCodeEventCollector() : _code_blobs(NULL) {
  if (JvmtiExport::should_post_dynamic_code_generated()) {
    setup_jvmti_thread_state();
  }
}

// iterate over any code blob descriptors collected and post a 
// DYNAMIC_CODE_GENERATED event to the profiler.
JvmtiDynamicCodeEventCollector::~JvmtiDynamicCodeEventCollector() {
  assert(!JavaThread::current()->owns_locks(), "all locks must be released to post deferred events");    
 // iterate over any code blob descriptors that we collected
 if (_code_blobs != NULL) {
   for (int i=0; i<_code_blobs->length(); i++) {
     JvmtiCodeBlobDesc* blob = _code_blobs->at(i);
     JvmtiExport::post_dynamic_code_generated(blob->name(), blob->code_begin(), blob->code_end());
     FreeHeap(blob);
   }
   delete _code_blobs;
 }
 unset_jvmti_thread_state();
}

// register a stub 
void JvmtiDynamicCodeEventCollector::register_stub(const char* name, address start, address end) {
 if (_code_blobs == NULL) {
   _code_blobs = new (ResourceObj::C_HEAP) GrowableArray<JvmtiCodeBlobDesc*>(1,true);
 }
 _code_blobs->append(new JvmtiCodeBlobDesc(name, start, end));
}

// Setup current thread to record vm allocated objects.
JvmtiVMObjectAllocEventCollector::JvmtiVMObjectAllocEventCollector() : _allocated(NULL) {
  if (JvmtiExport::should_post_vm_object_alloc()) {
    _enable = true; 
    setup_jvmti_thread_state();
  } else {
    _enable = false;
  }
}

// Post vm_object_alloc event for vm allocated objects visible to java
// world.
JvmtiVMObjectAllocEventCollector::~JvmtiVMObjectAllocEventCollector() {
  if (_allocated != NULL) {
    set_enabled(false);
    for (int i = 0; i < _allocated->length(); i++) {
oop obj=oop_at(i);
      if (ServiceUtil::visible_oop(obj)) {
        JvmtiExport::post_vm_object_alloc(JavaThread::current(), obj);	
      }
    }
    delete _allocated;
  }
  unset_jvmti_thread_state();
}

void JvmtiVMObjectAllocEventCollector::record_allocation(oop obj) {
  assert(is_enabled(), "VM object alloc event collector is not enabled");
  if (_allocated == NULL) {
_allocated=new(ResourceObj::C_HEAP)GrowableArray<uint64_t>(1,true);
  }
  _allocated->push(ALWAYS_POISON_OBJECTREF(objectRef(obj)).raw_value());
}

// GC support.
void JvmtiVMObjectAllocEventCollector::oops_do(OopClosure* f) {
  if (_allocated != NULL) {
    for(int i=_allocated->length() - 1; i >= 0; i--) {
if(oop_at(i)!=NULL){
f->do_oop((objectRef*)_allocated->adr_at(i));
      }
    }
  }
}

void JvmtiVMObjectAllocEventCollector::oops_do_for_all_threads(OopClosure* f) {
  // no-op if jvmti not enabled
  if (!JvmtiEnv::environments_might_exist()) {
    return;
  }

  // Runs at safepoint. So no need to acquire Threads_lock.
  for (JavaThread *jthr = Threads::first(); jthr != NULL; jthr = jthr->next()) {
    JvmtiThreadState *state = jthr->jvmti_thread_state();
    if (state != NULL) {
      JvmtiVMObjectAllocEventCollector *collector;
      collector = state->get_vm_object_alloc_event_collector();
      while (collector != NULL) {
        collector->oops_do(f);
        collector = (JvmtiVMObjectAllocEventCollector *)collector->get_prev();
      }
    }
  }
}


// Disable collection of VMObjectAlloc events 
NoJvmtiVMObjectAllocMark::NoJvmtiVMObjectAllocMark() : _collector(NULL) {
  // a no-op if VMObjectAlloc event is not enabled
  if (!JvmtiExport::should_post_vm_object_alloc()) {
    return;
  }
  Thread* thread = Thread::current();
  if (thread != NULL && thread->is_Java_thread())  {
    JavaThread* current_thread = (JavaThread*)thread;
    JvmtiThreadState *state = current_thread->jvmti_thread_state();
    if (state != NULL) {
      JvmtiVMObjectAllocEventCollector *collector;
      collector = state->get_vm_object_alloc_event_collector();
      if (collector != NULL && collector->is_enabled()) {
        _collector = collector;
        _collector->set_enabled(false);
      }
    }
  }
}

// Re-Enable collection of VMObjectAlloc events (if previously enabled)
NoJvmtiVMObjectAllocMark::~NoJvmtiVMObjectAllocMark() {
  if (was_enabled()) {
    _collector->set_enabled(true);
  }
};

JvmtiGCMarker::JvmtiGCMarker(bool full, bool pre_iterate_collection) : _full(full), _invocation_count(0), _pre_iterate_collection(pre_iterate_collection) {
  assert(Thread::current()->is_VM_thread() ||
         Thread::current()->is_GenPauselessGC_thread(), "wrong thread");

  // if there aren't any JVMTI environments then nothing to do
  if (!JvmtiEnv::environments_might_exist()) {
    return;
  }

  // GarbageCollectionStart event posted from VM thread - okay because
  // JVMTI is clear that the "world is stopped" and callback shouldn't
  // try to call into the VM.
  if (JvmtiExport::should_post_garbage_collection_start()) {
    JvmtiExport::post_garbage_collection_start();
  }

  // if "full" is false it probably means this is a scavenge of the young
  // generation. However it could turn out that a "full" GC is required
  // so we record the number of collections so that it can be checked in
  // the destructor.
  if (!_full) {
if(Universe::heap()->kind()==CollectedHeap::GenPauselessHeap){
      _invocation_count = GPGC_Heap::heap()->actual_old_collections();
    } else if (Universe::heap()->kind() == CollectedHeap::GenCollectedHeap) {
      GenCollectedHeap* gch = GenCollectedHeap::heap();
      assert(gch->n_gens() == 2, "configuration not recognized");
      _invocation_count = (unsigned int)gch->get_gen(1)->stat_record()->invocations;
    } else {
if(Universe::heap()->kind()==CollectedHeap::ParallelScavengeHeap){
        if (UseParallelOldGC) {
_invocation_count=PSParallelCompact::total_invocations();
        } else {
          _invocation_count = PSMarkSweep::total_invocations();
        }
      } else {
        // Only full collections with PGC
        ShouldNotReachHere();
      }
    }
  }

  // Do clean up tasks that need to be done at a safepoint
  JvmtiEnvBase::check_for_periodic_clean_up();
}
  
JvmtiGCMarker::~JvmtiGCMarker() {
  // if there aren't any JVMTI environments then nothing to do
  if (!JvmtiEnv::environments_might_exist()) {
    return;
  }

  // JVMTI notify gc finish
  if (JvmtiExport::should_post_garbage_collection_finish()) {
    JvmtiExport::post_garbage_collection_finish();
  }

  // we might have initially started out doing a scavenge of the young
  // generation but could have ended up doing a "full" GC - check the
  // GC count to see.
  if (!_full) {
if(Universe::heap()->kind()==CollectedHeap::GenPauselessHeap){
      uint64_t inv_count = GPGC_Heap::heap()->actual_old_collections();

      if (_invocation_count != inv_count) {
        _full = true;
      }
    } else if (Universe::heap()->kind() == CollectedHeap::GenCollectedHeap) {
      GenCollectedHeap* gch = GenCollectedHeap::heap();
      if (_invocation_count != (unsigned int)gch->get_gen(1)->stat_record()->invocations) {
        _full = true;      
      }
    } else {
if(Universe::heap()->kind()==CollectedHeap::ParallelScavengeHeap){
        uint64_t inv_count = 0;
        if (UseParallelOldGC) {
          inv_count = PSParallelCompact::total_invocations();
        } else {
inv_count=PSMarkSweep::total_invocations();
        }
  
        if (_invocation_count != inv_count) {
          _full = true;
        }
      } else {
        // Only full collections with PGC
        ShouldNotReachHere();
      }
    }
  }

  // Full collection probably means the perm generation has been GC'ed
  // so we clear the breakpoint cache.
  if (_full) {
    JvmtiCurrentBreakpoints::gc_epilogue();
  }

  // Notify heap/object tagging support
  if (!UseGenPauselessGC ) {
    // [G]PGC call pgc_ref_processing_epilogue
    JvmtiTagMap::gc_epilogue(_full);
  }
}

