/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <android/dlext.h>
#include <sys/mman.h>
#include <sys/wait.h>


#define ASSERT_DL_NOTNULL(ptr) \
    ASSERT_TRUE(ptr != NULL) << "dlerror: " << dlerror()

#define ASSERT_DL_ZERO(i) \
    ASSERT_EQ(0, i) << "dlerror: " << dlerror()

#define ASSERT_NOERROR(i) \
    ASSERT_NE(-1, i) << "errno: " << strerror(errno)


typedef int (*fn)(void);
#define LIBNAME "libdlext_test.so"
#define LIBNAME_NORELRO "libdlext_test_norelro.so"
#define LIBSIZE 1024*1024 // how much address space to reserve for it


class DlExtTest : public ::testing::Test {
protected:
  virtual void SetUp() {
    handle_ = NULL;
    // verify that we don't have the library loaded already
    ASSERT_EQ(NULL, dlsym(RTLD_DEFAULT, "getRandomNumber"));
    // call dlerror() to swallow the error, and check it was the one we wanted
    ASSERT_STREQ("undefined symbol: getRandomNumber", dlerror());
  }

  virtual void TearDown() {
    if (handle_ != NULL) {
      ASSERT_DL_ZERO(dlclose(handle_));
    }
  }

  void* handle_;
};

TEST_F(DlExtTest, ExtInfoNull) {
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, NULL);
  ASSERT_DL_NOTNULL(handle_);
  fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
  ASSERT_DL_NOTNULL(f);
  EXPECT_EQ(4, f());
}

TEST_F(DlExtTest, ExtInfoNoFlags) {
  android_dlextinfo extinfo;
  extinfo.flags = 0;
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, &extinfo);
  ASSERT_DL_NOTNULL(handle_);
  fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
  ASSERT_DL_NOTNULL(f);
  EXPECT_EQ(4, f());
}

TEST_F(DlExtTest, Reserved) {
  void* start = mmap(NULL, LIBSIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
  ASSERT_TRUE(start != MAP_FAILED);
  android_dlextinfo extinfo;
  extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
  extinfo.reserved_addr = start;
  extinfo.reserved_size = LIBSIZE;
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, &extinfo);
  ASSERT_DL_NOTNULL(handle_);
  fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
  ASSERT_DL_NOTNULL(f);
  EXPECT_GE(f, start);
  EXPECT_LT(reinterpret_cast<void*>(f),
            reinterpret_cast<char*>(start) + LIBSIZE);
  EXPECT_EQ(4, f());
}

TEST_F(DlExtTest, ReservedTooSmall) {
  void* start = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
  ASSERT_TRUE(start != MAP_FAILED);
  android_dlextinfo extinfo;
  extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
  extinfo.reserved_addr = start;
  extinfo.reserved_size = PAGE_SIZE;
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, &extinfo);
  EXPECT_EQ(NULL, handle_);
}

TEST_F(DlExtTest, ReservedHint) {
  void* start = mmap(NULL, LIBSIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
  ASSERT_TRUE(start != MAP_FAILED);
  android_dlextinfo extinfo;
  extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS_HINT;
  extinfo.reserved_addr = start;
  extinfo.reserved_size = LIBSIZE;
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, &extinfo);
  ASSERT_DL_NOTNULL(handle_);
  fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
  ASSERT_DL_NOTNULL(f);
  EXPECT_GE(f, start);
  EXPECT_LT(reinterpret_cast<void*>(f),
            reinterpret_cast<char*>(start) + LIBSIZE);
  EXPECT_EQ(4, f());
}

TEST_F(DlExtTest, ReservedHintTooSmall) {
  void* start = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
  ASSERT_TRUE(start != MAP_FAILED);
  android_dlextinfo extinfo;
  extinfo.flags = ANDROID_DLEXT_RESERVED_ADDRESS_HINT;
  extinfo.reserved_addr = start;
  extinfo.reserved_size = PAGE_SIZE;
  handle_ = android_dlopen_ext(LIBNAME, RTLD_NOW, &extinfo);
  ASSERT_DL_NOTNULL(handle_);
  fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
  ASSERT_DL_NOTNULL(f);
  EXPECT_TRUE(f < start || (reinterpret_cast<void*>(f) >=
                            reinterpret_cast<char*>(start) + PAGE_SIZE));
  EXPECT_EQ(4, f());
}

class DlExtRelroSharingTest : public DlExtTest {
protected:
  virtual void SetUp() {
    DlExtTest::SetUp();
    void* start = mmap(NULL, LIBSIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
    ASSERT_TRUE(start != MAP_FAILED);
    extinfo_.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
    extinfo_.reserved_addr = start;
    extinfo_.reserved_size = LIBSIZE;
    extinfo_.relro_fd = -1;

    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    snprintf(relro_file_, sizeof(relro_file_), "%s/local/tmp/libdlext_test.relro", android_data);
  }

  virtual void TearDown() {
    DlExtTest::TearDown();
    if (extinfo_.relro_fd != -1) {
      ASSERT_NOERROR(close(extinfo_.relro_fd));
    }
  }

  void CreateRelroFile(const char* lib) {
    int relro_fd = open(relro_file_, O_CREAT | O_RDWR | O_TRUNC, 0644);
    ASSERT_NOERROR(relro_fd);

    pid_t pid = fork();
    if (pid == 0) {
      // child process
      extinfo_.flags |= ANDROID_DLEXT_WRITE_RELRO;
      extinfo_.relro_fd = relro_fd;
      void* handle = android_dlopen_ext(lib, RTLD_NOW, &extinfo_);
      if (handle == NULL) {
        fprintf(stderr, "in child: %s\n", dlerror());
        exit(1);
      }
      exit(0);
    }

    // continuing in parent
    ASSERT_NOERROR(close(relro_fd));
    ASSERT_NOERROR(pid);
    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));

    // reopen file for reading so it can be used
    relro_fd = open(relro_file_, O_RDONLY);
    ASSERT_NOERROR(relro_fd);
    extinfo_.flags |= ANDROID_DLEXT_USE_RELRO;
    extinfo_.relro_fd = relro_fd;
  }

  void TryUsingRelro(const char* lib) {
    handle_ = android_dlopen_ext(lib, RTLD_NOW, &extinfo_);
    ASSERT_DL_NOTNULL(handle_);
    fn f = reinterpret_cast<fn>(dlsym(handle_, "getRandomNumber"));
    ASSERT_DL_NOTNULL(f);
    EXPECT_EQ(4, f());
  }

  android_dlextinfo extinfo_;
  char relro_file_[PATH_MAX];
};

TEST_F(DlExtRelroSharingTest, ChildWritesGoodData) {
  ASSERT_NO_FATAL_FAILURE(CreateRelroFile(LIBNAME));
  ASSERT_NO_FATAL_FAILURE(TryUsingRelro(LIBNAME));
}

TEST_F(DlExtRelroSharingTest, ChildWritesNoRelro) {
  ASSERT_NO_FATAL_FAILURE(CreateRelroFile(LIBNAME_NORELRO));
  ASSERT_NO_FATAL_FAILURE(TryUsingRelro(LIBNAME_NORELRO));
}

TEST_F(DlExtRelroSharingTest, RelroFileEmpty) {
  int relro_fd = open(relro_file_, O_CREAT | O_RDWR | O_TRUNC, 0644);
  ASSERT_NOERROR(relro_fd);
  ASSERT_NOERROR(close(relro_fd));

  ASSERT_NO_FATAL_FAILURE(TryUsingRelro(LIBNAME));
}
