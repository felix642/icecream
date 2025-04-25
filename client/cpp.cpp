/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 *
 * Run the preprocessor.  Client-side only.
 **/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <fstream>

#include "client.h"

using namespace std;

bool dcc_is_preprocessed(const string &sfile)
{
    if (sfile.size() < 3) {
        return false;
    }

    int last = sfile.size() - 1;

    if ((sfile[last - 1] == '.') && (sfile[last] == 'i')) {
        return true;    // .i
    }

    if ((sfile[last - 2] == '.') && (sfile[last - 1] == 'i') && (sfile[last] == 'i')) {
        return true;    // .ii
    }

    return false;
}

std::string trim(const std::string& str) {
    const auto strBegin = str.find_first_not_of(" \t\r\n");
    if (strBegin == std::string::npos) return "";

    const auto strEnd = str.find_last_not_of(" \t\r\n");
    return str.substr(strBegin, strEnd - strBegin + 1);
}

std::string unquote(const std::string& str) {
    if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
        return str.substr(1, str.size() - 2);
    }
    return str;
}

bool extractJsonStringField(const std::string& line, const std::string& key, std::string& value) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = line.find(searchKey);
    if (keyPos == std::string::npos) return false;

    size_t colonPos = line.find(':', keyPos);
    if (colonPos == std::string::npos) return false;

    size_t startQuote = line.find('"', colonPos + 1);
    if (startQuote == std::string::npos) return false;

    size_t endQuote = line.find('"', startQuote + 1);
    if (endQuote == std::string::npos) return false;

    value = unquote(line.substr(startQuote, endQuote - startQuote + 1));
    return true;
}

std::string getCompileCommand(const std::string& compileCommandsPath, const std::string& targetFilePath) {
    std::ifstream file(compileCommandsPath);
    if (!file) {
        throw std::runtime_error("Cannot open compile_commands.json file.");
    }

    std::string line;
    std::string currentFilePath;
    std::string currentCommand;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);
        std::string value;

        if (extractJsonStringField(trimmed, "file", value)) {
            currentFilePath = value;
        } else if (extractJsonStringField(trimmed, "command", value)) {
            currentCommand = value;
        }

        if (!currentFilePath.empty() && !currentCommand.empty()) {
            if (currentFilePath == targetFilePath) {
                return currentCommand;
            }
            currentFilePath.clear();
            currentCommand.clear();
        }
    }

    throw std::runtime_error("Compile command for the specified file was not found.");
}


// TODO duplicated code from file_util.cpp
vector<string> split(const string &s, char delim) {
    vector<string> elems;
    stringstream ss(s);
    string item;
    while (getline(ss, item, delim)) {
        if (!item.empty()) {
            elems.push_back(item);
        }
    }
    return elems;
}

// TODO duplicated code from arg.cpp
inline int str_startswith(const char *head, const char *worm)
{
    return !strncmp(head, worm, strlen(head));
}

pid_t get_clang_tidy_config(CompileJob& job, int fdwrite, int fdread)
{
    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork:");
        return -1; /* probably */
    }

    if (pid != 0) {
        /* Parent.  Close the write fd.  */
        if (fdwrite > -1) {
            if ((-1 == close(fdwrite)) && (errno != EBADF)){
                log_perror("close() failed");
            }
        }

        return pid;
    }

    /* Child.  Close the read fd, in case we have one.  */
    if (fdread > -1) {
        if ((-1 == close(fdread)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    int ret = dcc_ignore_sigpipe(0);

    if (ret) {  /* set handler back to default */
        _exit(ret);
    }


    char **argv;
    argv = new char*[3 + 1];
    argv[0] = strdup(find_compiler(job).c_str());
    argv[1] = strdup("--dump-config");
    argv[2] = strdup(job.inputFile().c_str());
    argv[3] = nullptr;

    if (fdwrite != STDOUT_FILENO) {
        /* Ignore failure */
        close(STDOUT_FILENO);
        dup2(fdwrite, STDOUT_FILENO);
        close(fdwrite);
    }

    execv(find_compiler(job).c_str(), argv);
    int exitcode = ( errno == ENOENT ? 127 : 126 );
    ostringstream errmsg;
    errmsg << "execv " << argv[0] << " failed";
    log_perror(errmsg.str());
    _exit(exitcode);
    return -1;
}

/**
 * If the input filename is a plain source file rather than a
 * preprocessed source file, then preprocess it to a temporary file
 * and return the name in @p cpp_fname.
 *
 * The preprocessor may still be running when we return; you have to
 * wait for @p cpp_fid to exit before the output is complete.  This
 * allows us to overlap opening the TCP socket, which probably doesn't
 * use many cycles, with running the preprocessor.
 **/
pid_t call_cpp(CompileJob &job, int fdwrite, int fdread)
{
    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork:");
        return -1; /* probably */
    }

    if (pid != 0) {
        /* Parent.  Close the write fd.  */
        if (fdwrite > -1) {
            if ((-1 == close(fdwrite)) && (errno != EBADF)){
                log_perror("close() failed");
            }
        }

        return pid;
    }

    /* Child.  Close the read fd, in case we have one.  */
    if (fdread > -1) {
        if ((-1 == close(fdread)) && (errno != EBADF)){
            log_perror("close failed");
        }
    }

    int ret = dcc_ignore_sigpipe(0);

    if (ret) {  /* set handler back to default */
        _exit(ret);
    }

    char **argv;

    if (dcc_is_preprocessed(job.inputFile())) {
        /* already preprocessed, great.
           write the file to the fdwrite (using cat) */
        argv = new char*[2 + 1];
        argv[0] = strdup("/bin/cat");
        argv[1] = strdup(job.inputFile().c_str());
        argv[2] = nullptr;
    } else {
        list<string> flags = job.localFlags();
        appendList(flags, job.restFlags());

        for (list<string>::iterator it = flags.begin(); it != flags.end();) {
            /* This has a duplicate meaning. it can either include a file
               for preprocessing or a precompiled header. decide which one.  */
            if ((*it) == "-include") {
                ++it;

                if (it != flags.end()) {
                    std::string p = (*it);

                    if (access(p.c_str(), R_OK) < 0 && access((p + ".gch").c_str(), R_OK) == 0) {
                        // PCH is useless for preprocessing, ignore the flag.
                        list<string>::iterator o = --it;
                        ++it;
                        flags.erase(o);
                        o = it++;
                        flags.erase(o);
                    }
                }
            } else if ((*it) == "-include-pch") {
                list<string>::iterator o = it;
                ++it;
                if (it != flags.end()) {
                    std::string p = (*it);
                    if (access(p.c_str(), R_OK) == 0) {
                        // PCH is useless for preprocessing (and probably slows things down), ignore the flag.
                        flags.erase(o);
                        o = it++;
                        flags.erase(o);
                    }
                }
            } else if ((*it) == "-fpch-preprocess") {
                // This would add #pragma GCC pch_preprocess to the preprocessed output, which would make
                // the remote GCC try to load the PCH directly and fail. Just drop it. This may cause a build
                // failure if the -include check above failed to detect usage of a PCH file (e.g. because
                // it needs to be found in one of the -I paths, which we don't check) and the header file
                // itself doesn't exist.
                flags.erase(it++);
            } else if ((*it) == "-fmodules" || (*it) == "-fcxx-modules" || (*it) == "-fmodules-ts"
                || (*it).find("-fmodules-cache-path=") == 0) {
                // Clang modules, handle like with PCH, remove the flags and compile remotely
                // without them.
                flags.erase(it++);
            } else {
                ++it;
            }
        }

        int argc = flags.size();
        argc++; // the program
        argc += 3; // -E -C file.i
        argc += 1; // -frewrite-includes / -fdirectives-only
        argv = new char*[argc + 1];
        argv[0] = strdup(find_compiler(job).c_str());
        int i = 1;

        for (list<string>::const_iterator it = flags.begin(); it != flags.end(); ++it) {
            argv[i++] = strdup(it->c_str());
        }

        argv[i++] = strdup("-E");
        if(compiler_is_clang_tidy(job)) {
            argv[i++] = strdup("-C");
        }

        argv[i++] = strdup(job.inputFile().c_str());

        if (compiler_only_rewrite_includes(job)) {
            if( compiler_is_clang(job)|| compiler_is_clang_tidy(job) ) {
                argv[i++] = strdup("-frewrite-includes");
            } else { // gcc
                argv[i++] = strdup("-fdirectives-only");
            }
        }
        
        argv[i++] = nullptr;
    }

    // Assume that both clang(++) and clang-tidy are located in the same directory.
    string compiler = argv[0];
    if(compiler_is_clang_tidy(job)) {
        size_t pos = compiler.find("-tidy");
        if (pos != string::npos) {
            CompileJob::Language lang = job.language();
            if(lang == CompileJob::Lang_CXX) {
                compiler.replace(pos, 5, "++");
            } else if (lang == CompileJob::Lang_C) {
                compiler.replace(pos, 5, "");
            } else {
                ostringstream errmsg;
                errmsg << "Unknown language " << lang;
                log_perror(errmsg.str());
                assert(0);
            }
        }
    }

    string argstxt = compiler;
    for( int i = 1; argv[ i ] != nullptr; ++i ) {
        argstxt += ' ';
        argstxt += argv[ i ];
    }
    trace() << "preparing source to send: " << argstxt << endl;

    if (fdwrite != STDOUT_FILENO) {
        /* Ignore failure */
        close(STDOUT_FILENO);
        dup2(fdwrite, STDOUT_FILENO);
        close(fdwrite);
    }

    dcc_increment_safeguard(SafeguardStepCompiler);

    std::vector<char *> filteredArgs;
    filteredArgs.push_back(&compiler[0]);

    for (int i = 1; argv[ i ] != nullptr; ++i) {
        std::string arg = argv[i];

        if (arg.find("--checks") != string::npos) {
            continue;
        }

        if (arg.find("--warnings-as-errors") != string::npos) {
            continue;
        }

        if(arg.find("--extra-arg") != string::npos) {
            continue;
        }

        if(arg == "-p") {
            string compile_command_folder_path = argv[++i];
            string compile_command_file_path = compile_command_folder_path + "/compile_commands.json";
            string compile_command = getCompileCommand(compile_command_file_path, job.inputFile());
            std::vector<string> splitted_compile_command = split(compile_command, ' ');
            for(auto iter = splitted_compile_command.begin(); iter < splitted_compile_command.end(); ++iter) {
                if (str_startswith("-I", iter->data())) {
                    filteredArgs.push_back(strdup(iter->data()));
                } else if (str_startswith("-isystem", iter->data())) {
                    filteredArgs.push_back(strdup(iter->data()));
                    iter++;
                    filteredArgs.push_back(strdup(iter->data()));
                } else if (str_startswith("-std=", iter->data())) {
                    filteredArgs.push_back(strdup(iter->data()));
                }
            }
            
            if(chdir(compile_command_folder_path.c_str()) != 0) {
                ostringstream errmsg;
                errmsg << "Failed to chdir to directory " <<  compile_command_folder_path << '\n';
                log_perror(errmsg.str());
            }

            continue;
        }

        filteredArgs.push_back(argv[i]);
    }
    filteredArgs.push_back(nullptr);
    
    execv(compiler.c_str(), filteredArgs.data());
    int exitcode = ( errno == ENOENT ? 127 : 126 );
    ostringstream errmsg;
    errmsg << "execv " << argv[0] << " failed";
    log_perror(errmsg.str());
    _exit(exitcode);
}
