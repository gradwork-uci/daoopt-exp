// Copyright 2013 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BASE_MUTEX_H_
#define BASE_MUTEX_H_

#include <stdlib.h>
#if defined(WINDOWS)
#include <windows.h>
typedef SRWLOCK pthread_rwlock_t;
typedef int pthread_rwlockattr_t;
static int pthread_rwlock_init(pthread_rwlock_t *l, pthread_rwlockattr_t *a)
{
	(void) a;
	InitializeSRWLock(l);

	return 0;
}

static int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
	(void) *l;
	return 0;
}

static int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
//	pthread_testcancel();
	AcquireSRWLockShared(l);

	return 0;
}

static int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
//	pthread_testcancel();
	AcquireSRWLockExclusive(l);

	return 0;
}

static int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
	void *state = *(void **)l;

	if (state == (void *) 1)
	{
		/* Known to be an exclusive lock */
		ReleaseSRWLockExclusive(l);
	}
	else
	{
		/* A shared unlock will work */
		ReleaseSRWLockShared(l);
	}

	return 0;
}
#else
#include <pthread.h>
#endif

#include "base/logging.h"
#include "base/macros.h"

// Basic mutex wrapper around a pthread RW lock.
class Mutex {
 public:
  inline Mutex() {
    CHECK(pthread_rwlock_init(&lock_, nullptr) == 0);
  }
  inline Mutex(base::LinkerInitialized) {  // NOLINT
    CHECK(pthread_rwlock_init(&lock_, nullptr) == 0);
  }
  inline ~Mutex() {
    CHECK(pthread_rwlock_destroy(&lock_) == 0);
  }
  inline void Lock() { WriterLock(); }
  inline void Unlock() { WriterUnlock(); }
  inline void ReaderLock() {
    CHECK(pthread_rwlock_rdlock(&lock_) == 0);
  }
  inline void ReaderUnlock() {
    CHECK(pthread_rwlock_unlock(&lock_) == 0);
  }
  inline void WriterLock() {
    CHECK(pthread_rwlock_wrlock(&lock_) == 0);
  }
  inline void WriterUnlock() {
    CHECK(pthread_rwlock_unlock(&lock_) == 0);
  }

 private:
  pthread_rwlock_t lock_;

  DISALLOW_COPY_AND_ASSIGN(Mutex);
};

// Scoped locking helpers.

class MutexLock {
 public:
  explicit MutexLock(Mutex *lock) : lock_(lock) { lock_->Lock(); }
  ~MutexLock() { lock_->Unlock(); }

 private:
  Mutex *lock_;

  DISALLOW_COPY_AND_ASSIGN(MutexLock);
};

class WriterMutexLock {
 public:
  explicit WriterMutexLock(Mutex *lock) : lock_(lock) { lock_->WriterLock(); }
  ~WriterMutexLock() { lock_->WriterUnlock(); }

 private:
  Mutex *lock_;

  DISALLOW_COPY_AND_ASSIGN(WriterMutexLock);
};

#endif  // BASE_MUTEX_H_
