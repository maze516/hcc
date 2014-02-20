#include<type_traits>
#pragma once

#include <string.h>
template<class T>
class GMACAllocator
{
 public:
  typedef T value_type;
  T* allocate(unsigned n) {
    T *p = NULL;
    ecl_error ret = eclMalloc(
	(void**)(const_cast<T**>(&p)), n * sizeof(T));
    assert(ret == eclSuccess);
    return p;
  }
};

template<class T>
class GMACDeleter {
 public:
  void operator()(T* ptr) {
    ecl_error ret = eclFree(
        const_cast<void*>(reinterpret_cast<const void*>(ptr)));
    assert(ret == eclSuccess);
  }
};

// Dummy interface that looks somewhat like std::shared_ptr<T>
template <typename T>
class _data {
 public:
  _data() = delete;
  _data(const _data& d) restrict(cpu, amp):p_(d.p_) {}
  __attribute__((annotate("user_deserialize")))
  explicit _data(__global T* t) restrict(cpu, amp) { p_ = t; }
  __global T* get(void) const restrict(cpu, amp) { return p_; }
  void reset(__global T *t = NULL) restrict(cpu, amp) { p_ = t; }
 private:
  __global T* p_;
};

// Wrap a shared pointer to the gmac buffer and the cache state. Act
// as if a shared pointer to the gmac buffer itself.
template <typename T>
class _data_host {
 private:
  //emum used to track the cache state.
  //HOST_OWNED: Most up to date version in the home location and will need to
  //be copied back to the gmac buffer before calling a kernel.
  //GMAC_OWNED: data was used in a kernel invocation and is presumed to have
  //been dirtied by it. Most up to date version will be in the gmac buffer.
  //SHARED: Data has been copied from the gmac buffer back to the home location
  //but has not been modified yet. Both buffers have the most up to date version.
  typedef enum {
    HOST_OWNED,
    GMAC_OWNED,
    SHARED
  } cache_state;
  std::shared_ptr<T> gmac_buffer;
  __attribute__((cpu)) std::shared_ptr<cache_state> state_ptr;
  __attribute__((cpu)) T* home_ptr;
  __attribute__((cpu)) size_t buffer_size;

 public:
  _data_host(T* cache, T* home, size_t size) :
   gmac_buffer(cache), home_ptr(home), buffer_size(size) {
    state_ptr = new cache_state;
    *state_ptr = HOST_OWNED;
  }
  
  template <class Deleter>
  _data_host(T* cache, Deleter d, T* home, size_t size) :
   gmac_buffer(cache, d), home_ptr(home), state_ptr(new cache_state), buffer_size(size) {
    *state_ptr = HOST_OWNED;
  }
  
  _data_host(T* cache) :
   gmac_buffer(cache), home_ptr(nullptr), buffer_size(0) {
    state_ptr = new cache_state;
    *state_ptr = GMAC_OWNED;
  }
  
  template <class Deleter>
  _data_host(T* cache, Deleter d) :
   gmac_buffer(cache, d), home_ptr(nullptr), buffer_size(0) {
    state_ptr = new cache_state;
    *state_ptr = GMAC_OWNED;
  }

  _data_host(const _data_host &other) :
    gmac_buffer(other.gmac_buffer), state_ptr(other.state_ptr),
    home_ptr(other.home_ptr), buffer_size(other.buffer_size) {}

  _data_host(std::nullptr_t x = nullptr):gmac_buffer(nullptr), buffer_size(0) {}

  void reset() {
    gmac_buffer.reset();
    state_ptr.reset();
  }

//The host buffer was modified without going through the array_view interface.
//Set it host owned so we know it is dirty and will copy it back to the gmac
//buffer when we serialize.
  void refresh() const { *state_ptr = HOST_OWNED; }

//If the cache is currently owned by the gmac buffer, copy it back to the host
//buffer and change the state to shared.
  void synchronize() const {
    if (*state_ptr == GMAC_OWNED) {
      memcpy(const_cast<void*>(reinterpret_cast<const void*>(home_ptr)),
              reinterpret_cast<const void*>(gmac_buffer.get()), buffer_size);
      *state_ptr = SHARED;
    }
  }

//Check the gmac buffer use count to see if we are the last copy of it. This
//is used to check if we are the last copy of an array_view and need to
//implicitly synchronize.
  bool is_last() const { return gmac_buffer.unique(); }
  
//Return the home location ptr, synchronizing first if necessary. The pointer
//returned is mutable, so we set it as host owned. If this is an array or array_view
//without a host buffer just return a pointer to the gmac buffer.
  T* get() const {
    if (home_ptr) {
      synchronize();
      *state_ptr = HOST_OWNED;
      return home_ptr;
    } else {
      return gmac_buffer.get();
    }
  }

//Return the home location ptr, synchronizing first if necessary. The pointer
//returned is const, so we can leave the cache in a shared state. If this is
//an array or array_view without a host buffer just return a pointer to the gmac buffer.
  const T* get() {
    if (home_ptr) {
      synchronize();
      return home_ptr;
    } else {
      return gmac_buffer.get();
    }
  }

//This is the serialization done by the runtime when an object is used in as a
//kernel argument. By overloading it we can do special work here instead of 
//just using the compiler provided version. First, we copy the data back to
//the gmac buffer if the cache is currently owned by the host. Instead of
//appending in the normal way we append the gmac buffer pointer. This removes
//the cache and shared pointer cruft, and the gmac runtime will translate the
//pointer to a ponter to global memory in the kernel.
  __attribute__((annotate("serialize")))
  void __cxxamp_serialize(Serialize& s) const {
    if (home_ptr && *state_ptr == HOST_OWNED) {
      memcpy(reinterpret_cast<void*>(gmac_buffer.get()),
              reinterpret_cast<const void*>(home_ptr), buffer_size);
    }
    *state_ptr = GMAC_OWNED;
    s.AppendPtr((const void *)gmac_buffer.get());
  }
};
