//===-- FileSystem.h --------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Host_FileSystem_h
#define liblldb_Host_FileSystem_h

#include "lldb/Host/FileSpec.h"
#include "lldb/Utility/Error.h"
#include "llvm/Support/Chrono.h"

#include "lldb/lldb-types.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

namespace lldb_private {
class FileSystem {
public:
  static const char *DEV_NULL;
  static const char *PATH_CONVERSION_ERROR;

  static Error Symlink(const FileSpec &src, const FileSpec &dst);
  static Error Readlink(const FileSpec &src, FileSpec &dst);

  static Error ResolveSymbolicLink(const FileSpec &src, FileSpec &dst);

  /// Wraps ::fopen in a platform-independent way. Once opened, FILEs can be
  /// manipulated and closed with the normal ::fread, ::fclose, etc. functions.
  static FILE *Fopen(const char *path, const char *mode);

  static llvm::sys::TimePoint<> GetModificationTime(const FileSpec &file_spec);
};
}

#endif
