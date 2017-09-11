//===- MinGW/Driver.cpp ---------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// GNU ld style linker driver for COFF currently supporting mingw-w64.
///
//===----------------------------------------------------------------------===//

#include "lld/Driver/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#endif

using namespace lld;
using namespace llvm;

// Create OptTable
enum {
  OPT_INVALID = 0,
#define OPTION(_1, _2, ID, _4, _5, _6, _7, _8, _9, _10, _11, _12) OPT_##ID,
#include "Options.inc"
#undef OPTION
};

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) static const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const opt::OptTable::Info InfoTable[] = {
#define OPTION(X1, X2, ID, KIND, GROUP, ALIAS, X7, X8, X9, X10, X11, X12)      \
  {X1, X2, X10,         X11,         OPT_##ID, opt::Option::KIND##Class,       \
   X9, X8, OPT_##GROUP, OPT_##ALIAS, X7,       X12},
#include "Options.inc"
#undef OPTION
};

namespace {
class MinGWOptTable : public opt::OptTable {
public:
  MinGWOptTable() : OptTable(InfoTable, false) {}
  opt::InputArgList parse(ArrayRef<const char *> Argv);
};
} // namespace

LLVM_ATTRIBUTE_NORETURN static void error(const Twine &Msg) {
  errs() << Msg << "\n";
  exit(1);
}

// Find a file by concatenating given paths.
static Optional<std::string> findFile(StringRef Path1, const Twine &Path2) {
  SmallString<128> S;
  sys::path::append(S, Path1, Path2);
  if (sys::fs::exists(S))
    return S.str().str();
  return None;
}

// This is for -lfoo. We'll look for libfoo.dll.a or libfoo.a from search paths.
static std::string
searchLibrary(StringRef Name, ArrayRef<StringRef> SearchPaths, bool BStatic) {
  if (Name.startswith(":")) {
    for (StringRef Dir : SearchPaths)
      if (Optional<std::string> S = findFile(Dir, Name.substr(1)))
        return *S;
    error("unable to find library -l" + Name);
  }

  for (StringRef Dir : SearchPaths) {
    if (!BStatic)
      if (Optional<std::string> S = findFile(Dir, "lib" + Name + ".dll.a"))
        return *S;
    if (Optional<std::string> S = findFile(Dir, "lib" + Name + ".a"))
      return *S;
  }
  error("unable to find library -l" + Name);
}

opt::InputArgList MinGWOptTable::parse(ArrayRef<const char *> Argv) {
  unsigned MissingIndex;
  unsigned MissingCount;

  SmallVector<const char *, 256> Vec(Argv.data(), Argv.data() + Argv.size());
  opt::InputArgList Args = this->ParseArgs(Vec, MissingIndex, MissingCount);

  if (MissingCount)
    error(StringRef(Args.getArgString(MissingIndex)) + ": missing argument");
  for (auto *Arg : Args.filtered(OPT_UNKNOWN))
    error("unknown argument: " + Arg->getSpelling());
  if (!Args.hasArgNoClaim(OPT_INPUT) && !Args.hasArgNoClaim(OPT_l))
    error("no input files");
  return Args;
}

// Convert Unix-ish command line arguments to Windows-ish ones and
// then call coff::link.
bool mingw::link(ArrayRef<const char *> ArgsArr, raw_ostream &Diag) {
  MinGWOptTable Parser;
  opt::InputArgList Args = Parser.parse(ArgsArr.slice(1));

  std::vector<std::string> LinkArgs;
  auto Add = [&](const Twine &S) { LinkArgs.push_back(S.str()); };

  Add("lld-link");

  if (auto *A = Args.getLastArg(OPT_entry))
    Add("-entry:" + StringRef(A->getValue()));
  if (auto *A = Args.getLastArg(OPT_subs))
    Add("-subsystem:" + StringRef(A->getValue()));
  if (auto *A = Args.getLastArg(OPT_out_implib))
    Add("-implib:" + StringRef(A->getValue()));
  if (auto *A = Args.getLastArg(OPT_stack))
    Add("-stack:" + StringRef(A->getValue()));

  if (auto *A = Args.getLastArg(OPT_o))
    Add("-out:" + StringRef(A->getValue()));
  else if (Args.hasArg(OPT_shared))
    Add("-out:a.dll");
  else
    Add("-out:a.exe");

  if (Args.hasArg(OPT_shared))
    Add("-dll");

  if (auto *A = Args.getLastArg(OPT_m)) {
    StringRef S = A->getValue();
    if (S == "i386pe")
      Add("-machine:x86");
    else if (S == "i386pep")
      Add("-machine:x64");
    else if (S == "thumb2pe")
      Add("-machine:arm");
    else if (S == "arm64pe")
      Add("-machine:arm64");
    else
      error("unknown parameter: -m" + S);
  }

  for (auto *A : Args.filtered(OPT_mllvm))
    Add("-mllvm:" + StringRef(A->getValue()));

  if (Args.getLastArgValue(OPT_m) == "i386pe")
    Add("-alternatename:__image_base__=___ImageBase");
  else
    Add("-alternatename:__image_base__=__ImageBase");

  std::vector<StringRef> SearchPaths;
  for (auto *A : Args.filtered(OPT_L))
    SearchPaths.push_back(A->getValue());

  for (auto *A : Args.filtered(OPT_INPUT, OPT_l)) {
    if (A->getOption().getUnaliasedOption().getID() == OPT_INPUT)
      Add(A->getValue());
    else
      Add(searchLibrary(A->getValue(), SearchPaths, Args.hasArg(OPT_Bstatic)));
  }

  if (Args.hasArg(OPT_verbose))
    Add("-verbose");

  if (Args.hasArg(OPT_verbose) || Args.hasArg(OPT__HASH_HASH_HASH))
    outs() << llvm::join(LinkArgs, " ") << "\n";

  if (Args.hasArg(OPT__HASH_HASH_HASH))
    return true;

  // Repack vector of strings to vector of const char pointers for coff::link.
  std::vector<const char *> Vec;
  for (const std::string &S : LinkArgs)
    Vec.push_back(S.c_str());
  return coff::link(Vec);
}
