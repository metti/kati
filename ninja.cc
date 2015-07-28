// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

#include "ninja.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "file_cache.h"
#include "flags.h"
#include "log.h"
#include "string_piece.h"
#include "stringprintf.h"
#include "strutil.h"
#include "var.h"
#include "version.h"

static size_t FindCommandLineFlag(StringPiece cmd, StringPiece name) {
  const size_t found = cmd.find(name);
  if (found == string::npos || found == 0)
    return string::npos;
  return found;
}

static StringPiece FindCommandLineFlagWithArg(StringPiece cmd,
                                              StringPiece name) {
  size_t index = FindCommandLineFlag(cmd, name);
  if (index == string::npos)
    return StringPiece();

  StringPiece val = TrimLeftSpace(cmd.substr(index + name.size()));
  index = val.find(name);
  while (index != string::npos) {
    val = TrimLeftSpace(val.substr(index + name.size()));
    index = val.find(name);
  }

  index = val.find_first_of(" \t");
  return val.substr(0, index);
}

static bool StripPrefix(StringPiece p, StringPiece* s) {
  if (!HasPrefix(*s, p))
    return false;
  *s = s->substr(p.size());
  return true;
}

size_t GetGomaccPosForAndroidCompileCommand(StringPiece cmdline) {
  size_t index = cmdline.find(' ');
  if (index == string::npos)
    return string::npos;
  StringPiece cmd = cmdline.substr(0, index);
  if (HasSuffix(cmd, "ccache")) {
    index++;
    size_t pos = GetGomaccPosForAndroidCompileCommand(cmdline.substr(index));
    return pos == string::npos ? string::npos : pos + index;
  }
  if (!StripPrefix("prebuilts/", &cmd))
    return string::npos;
  if (!StripPrefix("gcc/", &cmd) && !StripPrefix("clang/", &cmd))
    return string::npos;
  if (!HasSuffix(cmd, "gcc") && !HasSuffix(cmd, "g++") &&
      !HasSuffix(cmd, "clang") && !HasSuffix(cmd, "clang++")) {
    return string::npos;
  }

  StringPiece rest = cmdline.substr(index);
  return rest.find(" -c ") != string::npos ? 0 : string::npos;
}

static bool GetDepfileFromCommandImpl(StringPiece cmd, string* out) {
  if ((FindCommandLineFlag(cmd, " -MD") == string::npos &&
       FindCommandLineFlag(cmd, " -MMD") == string::npos) ||
      FindCommandLineFlag(cmd, " -c") == string::npos) {
    return false;
  }

  StringPiece mf = FindCommandLineFlagWithArg(cmd, " -MF");
  if (!mf.empty()) {
    mf.AppendToString(out);
    return true;
  }

  StringPiece o = FindCommandLineFlagWithArg(cmd, " -o");
  if (o.empty()) {
    ERROR("Cannot find the depfile in %s", cmd.as_string().c_str());
    return false;
  }

  StripExt(o).AppendToString(out);
  *out += ".d";
  return true;
}

bool GetDepfileFromCommand(string* cmd, string* out) {
  CHECK(!cmd->empty());
  if (!GetDepfileFromCommandImpl(*cmd, out))
    return false;

  // A hack for Android - llvm-rs-cc seems not to emit a dep file.
  if (cmd->find("bin/llvm-rs-cc ") != string::npos) {
    return false;
  }

  // TODO: A hack for Makefiles generated by automake.

  // A hack for Android to get .P files instead of .d.
  string p;
  StripExt(*out).AppendToString(&p);
  p += ".P";
  if (cmd->find(p) != string::npos) {
    const string rm_f = "; rm -f " + *out;
    const size_t found = cmd->find(rm_f);
    if (found == string::npos) {
      ERROR("Cannot find removal of .d file: %s", cmd->c_str());
    }
    cmd->erase(found, rm_f.size());
    return true;
  }

  // A hack for Android. For .s files, GCC does not use C
  // preprocessor, so it ignores -MF flag.
  string as = "/";
  StripExt(Basename(*out)).AppendToString(&as);
  as += ".s";
  if (cmd->find(as) != string::npos) {
    return false;
  }

  *cmd += "&& cp ";
  *cmd += *out;
  *cmd += ' ';
  *cmd += *out;
  *cmd += ".tmp ";
  *out += ".tmp";
  return true;
}

class NinjaGenerator {
 public:
  NinjaGenerator(const char* ninja_suffix, const char* ninja_dir, Evaluator* ev)
      : ce_(ev), ev_(ev), fp_(NULL), rule_id_(0) {
    ev_->set_avoid_io(true);
    shell_ = ev->EvalVar(kShellSym);
    if (g_goma_dir)
      gomacc_ = StringPrintf("%s/gomacc ", g_goma_dir);
    if (ninja_suffix) {
      ninja_suffix_ = ninja_suffix;
    }
    if (ninja_dir) {
      ninja_dir_ = ninja_dir;
    } else {
      ninja_dir_ = ".";
    }

    for (Symbol e : Vars::used_env_vars()) {
      shared_ptr<string> val = ev_->EvalVar(e);
      used_envs_.emplace(e.str(), *val);
    }
  }

  ~NinjaGenerator() {
    ev_->set_avoid_io(false);
  }

  void Generate(const vector<DepNode*>& nodes,
                bool build_all_targets,
                const string& orig_args) {
    GenerateEnvlist();
    GenerateNinja(nodes, build_all_targets, orig_args);
    GenerateShell();
  }

 private:
  string GenRuleName() {
    return StringPrintf("rule%d", rule_id_++);
  }

  StringPiece TranslateCommand(const char* in, string* cmd_buf) {
    const size_t orig_size = cmd_buf->size();
    bool prev_backslash = false;
    // Set space as an initial value so the leading comment will be
    // stripped out.
    char prev_char = ' ';
    char quote = 0;
    bool done = false;
    for (; *in && !done; in++) {
      switch (*in) {
        case '#':
          if (quote == 0 && isspace(prev_char)) {
            done = true;
          } else {
            *cmd_buf += *in;
          }
          break;

        case '\'':
        case '"':
        case '`':
          if (quote) {
            if (quote == *in)
              quote = 0;
          } else if (!prev_backslash) {
            quote = *in;
          }
          *cmd_buf += *in;
          break;

        case '$':
          *cmd_buf += "$$";
          break;

        case '\n':
          if (prev_backslash) {
            cmd_buf->resize(cmd_buf->size()-1);
          } else {
            *cmd_buf += ' ';
          }
          break;

        case '\\':
          *cmd_buf += '\\';
          break;

        default:
          *cmd_buf += *in;
      }

      if (*in == '\\') {
        prev_backslash = !prev_backslash;
      } else {
        prev_backslash = false;
      }

      prev_char = *in;
    }

    while (true) {
      char c = (*cmd_buf)[cmd_buf->size()-1];
      if (!isspace(c) && c != ';')
        break;
      cmd_buf->resize(cmd_buf->size() - 1);
    }

    return StringPiece(cmd_buf->data() + orig_size,
                       cmd_buf->size() - orig_size);
  }

  bool GetDescriptionFromCommand(StringPiece cmd, string *out) {
    if (!HasPrefix(cmd, "echo ")) {
      return false;
    }
    cmd = cmd.substr(5, cmd.size());

    bool prev_backslash = false;
    char quote = 0;
    string out_buf;

    // Strip outer quotes, and fail if it is not a single echo command
    for (StringPiece::iterator in = cmd.begin(); in != cmd.end(); in++) {
      if (prev_backslash) {
        prev_backslash = false;
        out_buf += *in;
      } else if (*in == '\\') {
        prev_backslash = true;
        out_buf += *in;
      } else if (quote) {
        if (*in == quote) {
          quote = 0;
        } else {
          out_buf += *in;
        }
      } else {
        switch (*in) {
        case '\'':
        case '"':
        case '`':
          quote = *in;
          break;

        case '<':
        case '>':
        case '&':
        case '|':
        case ';':
          return false;

        default:
          out_buf += *in;
        }
      }
    }

    *out = out_buf;
    return true;
  }

  bool GenShellScript(const vector<Command*>& commands,
                      string* cmd_buf,
                      string* description) {
    bool got_descritpion = false;
    bool use_gomacc = false;
    bool should_ignore_error = false;
    for (const Command* c : commands) {
      if (!cmd_buf->empty()) {
        if (should_ignore_error) {
          *cmd_buf += " ; ";
        } else {
          *cmd_buf += " && ";
        }
      }
      should_ignore_error = c->ignore_error;

      const char* in = c->cmd->c_str();
      while (isspace(*in))
        in++;

      bool needs_subshell = commands.size() > 1;
      if (*in == '(') {
        needs_subshell = false;
      }

      if (needs_subshell)
        *cmd_buf += '(';

      size_t cmd_start = cmd_buf->size();
      StringPiece translated = TranslateCommand(in, cmd_buf);
      if (g_detect_android_echo && !got_descritpion && !c->echo &&
          GetDescriptionFromCommand(translated, description)) {
        got_descritpion = true;
        cmd_buf->resize(cmd_start);
        translated.clear();
      }
      if (translated.empty()) {
        *cmd_buf += "true";
      } else if (g_goma_dir) {
        size_t pos = GetGomaccPosForAndroidCompileCommand(translated);
        if (pos != string::npos) {
          cmd_buf->insert(cmd_start + pos, gomacc_);
          use_gomacc = true;
        }
      }

      if (c == commands.back() && c->ignore_error) {
        *cmd_buf += " ; true";
      }

      if (needs_subshell)
        *cmd_buf += ')';
    }
    return g_goma_dir && !use_gomacc;
  }

  void EmitDepfile(string* cmd_buf) {
    *cmd_buf += ' ';
    string depfile;
    bool result = GetDepfileFromCommand(cmd_buf, &depfile);
    cmd_buf->resize(cmd_buf->size()-1);
    if (!result)
      return;
    fprintf(fp_, " depfile = %s\n", depfile.c_str());
    fprintf(fp_, " deps = gcc\n");
  }

  void EmitNode(DepNode* node) {
    auto p = done_.insert(node->output);
    if (!p.second)
      return;

    // Removing this will fix auto_vars.mk, build_once.mk, and
    // command_vars.mk. However, this change will make
    // ninja_normalized_path2.mk fail and cause a lot of warnings for
    // Android build.
    if (node->cmds.empty() &&
        node->deps.empty() && node->order_onlys.empty() && !node->is_phony) {
      return;
    }

    StringPiece base = Basename(node->output.str());
    if (base != node->output.str()) {
      auto p = short_names_.emplace(Intern(base), node->output);
      if (!p.second) {
        // We generate shortcuts only for targets whose basename are unique.
        p.first->second = kEmptySym;
      }
    }

    vector<Command*> commands;
    ce_.Eval(node, &commands);

    string rule_name = "phony";
    bool use_local_pool = false;
    if (!commands.empty()) {
      rule_name = GenRuleName();
      fprintf(fp_, "rule %s\n", rule_name.c_str());

      string description = "build $out";
      string cmd_buf;
      use_local_pool |= GenShellScript(commands, &cmd_buf, &description);
      fprintf(fp_, " description = %s\n", description.c_str());
      EmitDepfile(&cmd_buf);

      // It seems Linux is OK with ~130kB and Mac's limit is ~250kB.
      // TODO: Find this number automatically.
      if (cmd_buf.size() > 100 * 1000) {
        fprintf(fp_, " rspfile = $out.rsp\n");
        fprintf(fp_, " rspfile_content = %s\n", cmd_buf.c_str());
        fprintf(fp_, " command = %s $out.rsp\n", shell_->c_str());
      } else {
        fprintf(fp_, " command = %s -c \"%s\"\n",
                shell_->c_str(), EscapeShell(cmd_buf).c_str());
      }
    }

    EmitBuild(node, rule_name);
    if (use_local_pool)
      fprintf(fp_, " pool = local_pool\n");

    for (DepNode* d : node->deps) {
      EmitNode(d);
    }
    for (DepNode* d : node->order_onlys) {
      EmitNode(d);
    }
  }

  string EscapeBuildTarget(Symbol s) {
    if (s.str().find_first_of("$: ") == string::npos)
      return s.str();
    string r;
    for (char c : s.str()) {
      switch (c) {
        case '$':
        case ':':
        case ' ':
          r += '$';
          // fall through.
        default:
          r += c;
      }
    }
    return r;
  }

  string EscapeShell(string s) {
    if (s.find_first_of("$`!\\\"") == string::npos)
      return s;
    string r;
    bool lastDollar = false;
    for (char c : s) {
      switch (c) {
        case '$':
          if (lastDollar) {
            r += c;
            lastDollar = false;
          } else {
            r += '\\';
            r += c;
            lastDollar = true;
          }
          break;
        case '`':
        case '"':
        case '!':
        case '\\':
          r += '\\';
          // fall through.
        default:
          r += c;
          lastDollar = false;
      }
    }
    return r;
  }

  void EmitBuild(DepNode* node, const string& rule_name) {
    fprintf(fp_, "build %s: %s",
            EscapeBuildTarget(node->output).c_str(),
            rule_name.c_str());
    vector<Symbol> order_onlys;
    for (DepNode* d : node->deps) {
      fprintf(fp_, " %s", EscapeBuildTarget(d->output).c_str());
    }
    if (!node->order_onlys.empty()) {
      fprintf(fp_, " ||");
      for (DepNode* d : node->order_onlys) {
        fprintf(fp_, " %s", EscapeBuildTarget(d->output).c_str());
      }
    }
    fprintf(fp_, "\n");
  }

  void EmitRegenRules(const string& orig_args) {
    if (!g_gen_regen_rule)
      return;

    fprintf(fp_, "rule regen_ninja\n");
    fprintf(fp_, " command = %s\n", orig_args.c_str());
    fprintf(fp_, " generator = 1\n");
    fprintf(fp_, " description = Regenerate ninja files due to dependency\n");
    fprintf(fp_, "build %s: regen_ninja", GetNinjaFilename().c_str());
    unordered_set<string> makefiles;
    MakefileCacheManager::Get()->GetAllFilenames(&makefiles);
    for (const string& makefile : makefiles) {
      fprintf(fp_, " %.*s", SPF(makefile));
    }
    // TODO: Add dependencies to directories read by $(wildcard)
    // or $(shell find).
    if (!used_envs_.empty())
      fprintf(fp_, " %s", GetEnvlistFilename().c_str());
    fprintf(fp_, "\n\n");

    if (used_envs_.empty())
      return;

    fprintf(fp_, "build .always_build: phony\n");
    fprintf(fp_, "rule regen_envlist\n");
    fprintf(fp_, " command = rm -f $out.tmp");
    for (const auto& p : used_envs_) {
      fprintf(fp_, " && echo %s=$$%s >> $out.tmp",
              p.first.c_str(), p.first.c_str());
    }
    if (g_error_on_env_change) {
      fprintf(fp_,
              " && (diff $out.tmp $out || "
              "(echo Environment variable changes are detected && false))\n");
    } else {
      fprintf(fp_, " && (diff $out.tmp $out || mv $out.tmp $out)\n");
    }
    fprintf(fp_, " restat = 1\n");
    fprintf(fp_, " generator = 1\n");
    fprintf(fp_, " description = Check $out\n");
    fprintf(fp_, "build %s: regen_envlist .always_build\n\n",
            GetEnvlistFilename().c_str());
  }

  string GetNinjaFilename() const {
    return StringPrintf("%s/build%s.ninja",
                        ninja_dir_.c_str(), ninja_suffix_.c_str());
  }

  string GetShellScriptFilename() const {
    return StringPrintf("%s/ninja%s.sh",
                        ninja_dir_.c_str(), ninja_suffix_.c_str());
  }

  string GetEnvlistFilename() const {
    return StringPrintf("%s/.kati_env%s",
                        ninja_dir_.c_str(), ninja_suffix_.c_str());
  }

  string GetLunchFilename() const {
    return StringPrintf("%s/.kati_lunch%s",
                        ninja_dir_.c_str(), ninja_suffix_.c_str());
  }

  void GenerateNinja(const vector<DepNode*>& nodes,
                     bool build_all_targets,
                     const string& orig_args) {
    fp_ = fopen(GetNinjaFilename().c_str(), "wb");
    if (fp_ == NULL)
      PERROR("fopen(build.ninja) failed");

    fprintf(fp_, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp_, "\n");

    if (!used_envs_.empty()) {
      fprintf(fp_, "# Environment variables used:\n");
      for (const auto& p : used_envs_) {
        fprintf(fp_, "# %s=%s\n", p.first.c_str(), p.second.c_str());
      }
      fprintf(fp_, "\n");
    }

    if (g_goma_dir) {
      fprintf(fp_, "pool local_pool\n");
      fprintf(fp_, " depth = %d\n\n", g_num_jobs);
    }

    EmitRegenRules(orig_args);

    for (DepNode* node : nodes) {
      EmitNode(node);
    }

    if (!build_all_targets) {
      CHECK(!nodes.empty());
      fprintf(fp_, "\ndefault %s\n", nodes.front()->output.c_str());
    }

    fprintf(fp_, "\n# shortcuts:\n");
    for (auto p : short_names_) {
      if (!p.second.empty() && !done_.count(p.first))
        fprintf(fp_, "build %s: phony %s\n", p.first.c_str(), p.second.c_str());
    }

    fclose(fp_);
  }

  void GenerateShell() {
    FILE* fp = fopen(GetShellScriptFilename().c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(ninja.sh) failed");

    shared_ptr<string> shell = ev_->EvalVar(kShellSym);
    if (shell->empty())
      shell = make_shared<string>("/bin/sh");
    fprintf(fp, "#!%s\n", shell->c_str());
    fprintf(fp, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp, "\n");
    if (ninja_dir_ == ".")
      fprintf(fp, "cd $(dirname \"$0\")\n");
    if (!ninja_suffix_.empty()) {
      fprintf(fp, "if [ -f %s ]; then\n export $(cat %s)\nfi\n",
              GetEnvlistFilename().c_str(), GetEnvlistFilename().c_str());
      fprintf(fp, "if [ -f %s ]; then\n export $(cat %s)\nfi\n",
              GetLunchFilename().c_str(), GetLunchFilename().c_str());
    }

    for (const auto& p : ev_->exports()) {
      if (p.second) {
        shared_ptr<string> val = ev_->EvalVar(p.first);
        fprintf(fp, "export %s=%s\n", p.first.c_str(), val->c_str());
      } else {
        fprintf(fp, "unset %s\n", p.first.c_str());
      }
    }

    fprintf(fp, "exec ninja -f %s ", GetNinjaFilename().c_str());
    if (g_goma_dir) {
      fprintf(fp, "-j500 ");
    }
    fprintf(fp, "\"$@\"\n");

    if (chmod(GetShellScriptFilename().c_str(), 0755) != 0)
      PERROR("chmod ninja.sh failed");
  }

  void GenerateEnvlist() {
    if (used_envs_.empty())
      return;
    FILE* fp = fopen(GetEnvlistFilename().c_str(), "wb");
    for (const auto& p : used_envs_) {
      fprintf(fp, "%s=%s\n", p.first.c_str(), p.second.c_str());
    }
    fclose(fp);
  }

  CommandEvaluator ce_;
  Evaluator* ev_;
  FILE* fp_;
  unordered_set<Symbol> done_;
  int rule_id_;
  string gomacc_;
  string ninja_suffix_;
  string ninja_dir_;
  unordered_map<Symbol, Symbol> short_names_;
  shared_ptr<string> shell_;
  map<string, string> used_envs_;
};

void GenerateNinja(const char* ninja_suffix,
                   const char* ninja_dir,
                   const vector<DepNode*>& nodes,
                   Evaluator* ev,
                   bool build_all_targets,
                   const string& orig_args) {
  NinjaGenerator ng(ninja_suffix, ninja_dir, ev);
  ng.Generate(nodes, build_all_targets, orig_args);
}
