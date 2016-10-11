/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include <execinfo.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

#include <png.h>

#include <Poco/Base64Encoder.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Exception.h>
#include <Poco/Format.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Process.h>
#include <Poco/RandomStream.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <Poco/Util/Application.h>

#include "Common.hpp"
#include "Util.hpp"
#include "Png.hpp"

// Callback functions for libpng

extern "C"
{
    static void user_write_status_fn(png_structp, png_uint_32, int)
    {
    }

    static void user_write_fn(png_structp png_ptr, png_bytep data, png_size_t length)
    {
        std::vector<char> *outputp = (std::vector<char> *) png_get_io_ptr(png_ptr);
        const size_t oldsize = outputp->size();
        outputp->resize(oldsize + length);
        std::memcpy(outputp->data() + oldsize, data, length);
    }

    static void user_flush_fn(png_structp)
    {
    }
}

volatile bool TerminationFlag = false;

namespace Util
{
namespace rng
{
    static std::random_device _rd;
    static std::mutex _rngMutex;
    static Poco::RandomBuf _randBuf;

    // Create the prng with a random-device for seed.
    // If we don't have a hardware random-device, we will get the same seed.
    // In that case we are better off with an arbitrary, but changing, seed.
    static std::mt19937_64 _rng = std::mt19937_64(_rd.entropy()
                                                ? _rd()
                                                : (clock() + getpid()));

    // A new seed is used to shuffle the sequence.
    // N.B. Always reseed after getting forked!
    void reseed()
    {
        _rng.seed(_rd.entropy() ? _rd() : (clock() + getpid()));
    }

    // Returns a new random number.
    unsigned getNext()
    {
        std::unique_lock<std::mutex> lock(_rngMutex);
        return _rng();
    }

    std::vector<char> getBytes(const size_t length)
    {
        std::vector<char> v(length);
        _randBuf.readFromDevice(v.data(), v.size());
        return v;
    }

    /// Generates a random string in Base64.
    /// Note: May contain '/' characters.
    std::string getB64String(const size_t length)
    {
        std::stringstream ss;
        Poco::Base64Encoder b64(ss);
        b64 << getBytes(length).data();
        return ss.str().substr(0, length);
    }

    /// Generates a random string suitable for
    /// file/directory names.
    std::string getFilename(const size_t length)
    {
        std::stringstream ss;
        Poco::Base64Encoder b64(ss);
        b64 << getBytes(length).data();
        std::string s = ss.str();
        std::replace(s.begin(), s.end(), '/', '_');
        return s.substr(0, length);
    }
}
}

namespace Util
{
    std::string encodeId(const unsigned number, const int padding)
    {
        std::ostringstream oss;
        oss << std::hex << std::setw(padding) << std::setfill('0') << number;
        return oss.str();
    }

    unsigned decodeId(const std::string& str)
    {
        unsigned id = 0;
        std::stringstream ss;
        ss << std::hex << str;
        ss >> id;
        return id;
    }

    /// Create a secure, random directory path.
    std::string createRandomDir(const std::string& path)
    {
        Poco::File(path).createDirectories();
        const auto name = rng::getFilename(64);
        Poco::File(Poco::Path(path, name)).createDirectories();
        return name;
    }

    std::string getTempFilePath(const std::string srcDir, const std::string& srcFilename)
    {
        const std::string srcPath = srcDir + '/' + srcFilename;
        const std::string dstPath = Poco::Path::temp() + encodeId(rng::getNext()) + '_' + srcFilename;
        Poco::File(srcPath).copyTo(dstPath);
        Poco::TemporaryFile::registerForDeletion(dstPath);
        return dstPath;
    }

    bool windowingAvailable()
    {
        return std::getenv("DISPLAY") != nullptr;
    }

    bool saveDataToFileSafely(std::string fileName, const char *data, size_t size)
    {
        const auto tempFileName = fileName + ".temp";
        std::fstream outStream(tempFileName, std::ios::out);

        // If we can't create the file properly, just remove it
        if (!outStream.good())
        {
            Log::error("Creating " + tempFileName + " failed, disk full?");
            // Try removing both just in case
            std::remove(tempFileName.c_str());
            std::remove(fileName.c_str());
            return false;
        }
        else
        {
            outStream.write(data, size);
            if (!outStream.good())
            {
                Log::error("Writing to " + tempFileName + " failed, disk full?");
                outStream.close();
                std::remove(tempFileName.c_str());
                std::remove(fileName.c_str());
                return false;
            }
            else
            {
                outStream.close();
                if (!outStream.good())
                {
                    Log::error("Closing " + tempFileName + " failed, disk full?");
                    std::remove(tempFileName.c_str());
                    std::remove(fileName.c_str());
                    return false;
                }
                else
                {
                    // Everything OK, rename the file to its proper name
                    if (std::rename(tempFileName.c_str(), fileName.c_str()) == 0)
                    {
                        Log::debug() << "Renaming " << tempFileName << " to " << fileName << " OK." << Log::end;
                        return true;
                    }
                    else
                    {
                        Log::error("Renaming " + tempFileName + " to " + fileName + " failed, disk full?");
                        std::remove(tempFileName.c_str());
                        std::remove(fileName.c_str());
                        return false;
                    }
                }
            }
        }
    }

    bool encodeBufferToPNG(unsigned char *pixmap, int width, int height, std::vector<char>& output, LibreOfficeKitTileMode mode)
    {

        return encodeSubBufferToPNG(pixmap, 0, 0, width, height, width, height, output, mode);
    }

    bool encodeSubBufferToPNG(unsigned char *pixmap, int startX, int startY, int width, int height,
                              int bufferWidth, int bufferHeight, std::vector<char>& output, LibreOfficeKitTileMode mode)
    {
        if (bufferWidth < width || bufferHeight < height)
            return false;

        png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

        png_infop info_ptr = png_create_info_struct(png_ptr);

        if (setjmp(png_jmpbuf(png_ptr)))
        {
            png_destroy_write_struct(&png_ptr, nullptr);
            return false;
        }

        png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

        png_set_write_fn(png_ptr, &output, user_write_fn, user_flush_fn);
        png_set_write_status_fn(png_ptr, user_write_status_fn);

        png_write_info(png_ptr, info_ptr);

        if (mode == LOK_TILEMODE_BGRA)
        {
            png_set_write_user_transform_fn (png_ptr, unpremultiply_data);
        }

        for (int y = 0; y < height; ++y)
        {
            size_t position = ((startY + y) * bufferWidth * 4) + (startX * 4);
            png_write_row(png_ptr, pixmap + position);
        }

        png_write_end(png_ptr, info_ptr);

        png_destroy_write_struct(&png_ptr, &info_ptr);

        return true;
    }

    const char *signalName(const int signo)
    {
        switch (signo)
        {
#define CASE(x) case SIG##x: return "SIG" #x
            CASE(HUP);
            CASE(INT);
            CASE(QUIT);
            CASE(ILL);
            CASE(ABRT);
            CASE(FPE);
            CASE(KILL);
            CASE(SEGV);
            CASE(PIPE);
            CASE(ALRM);
            CASE(TERM);
            CASE(USR1);
            CASE(USR2);
            CASE(CHLD);
            CASE(CONT);
            CASE(STOP);
            CASE(TSTP);
            CASE(TTIN);
            CASE(TTOU);
            CASE(BUS);
#ifdef SIGPOLL
            CASE(POLL);
#endif
            CASE(PROF);
            CASE(SYS);
            CASE(TRAP);
            CASE(URG);
            CASE(VTALRM);
            CASE(XCPU);
            CASE(XFSZ);
#ifdef SIGEMT
            CASE(EMT);
#endif
#ifdef SIGSTKFLT
            CASE(STKFLT);
#endif
#if defined(SIGIO) && SIGIO != SIGPOLL
            CASE(IO);
#endif
#ifdef SIGPWR
            CASE(PWR);
#endif
#ifdef SIGLOST
            CASE(LOST);
#endif
            CASE(WINCH);
#if defined(SIGINFO) && SIGINFO != SIGPWR
            CASE(INFO);
#endif
#undef CASE
        default:
            return "unknown";
        }
    }

    static
    void handleTerminationSignal(const int signal)
    {
        if (!TerminationFlag)
        {
            TerminationFlag = true;

            Log::signalLogPrefix();
            Log::signalLog(" Termination signal received: ");
            Log::signalLog(signalName(signal));
            Log::signalLog("\n");
        }
    }

    void setTerminationSignals()
    {
        struct sigaction action;

        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        action.sa_handler = handleTerminationSignal;

        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGQUIT, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
    }

    static char FatalGdbString[256] = { '\0' };

    static
    void handleFatalSignal(const int signal)
    {
        Log::signalLogPrefix();
        Log::signalLog(" Fatal signal received: ");
        Log::signalLog(signalName(signal));
        Log::signalLog("\n");

        if (std::getenv("LOOL_DEBUG"))
        {
            Log::signalLog(FatalGdbString);
            sleep(30);
        }

        struct sigaction action;

        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        action.sa_handler = SIG_DFL;

        sigaction(signal, &action, NULL);

        char header[32];
        sprintf(header, "Backtrace %d:\n", getpid());

        const int maxSlots = 50;
        void *backtraceBuffer[maxSlots];
        int numSlots = backtrace(backtraceBuffer, maxSlots);
        if (numSlots > 0)
        {
            char **symbols = backtrace_symbols(backtraceBuffer, numSlots);
            if (symbols != NULL)
            {
                struct iovec ioVector[maxSlots*2+1];
                ioVector[0].iov_base = (void*)header;
                ioVector[0].iov_len = std::strlen((const char*)ioVector[0].iov_base);
                for (int i = 0; i < numSlots; i++)
                {
                    ioVector[1+i*2+0].iov_base = symbols[i];
                    ioVector[1+i*2+0].iov_len = std::strlen((const char *)ioVector[1+i*2+0].iov_base);
                    ioVector[1+i*2+1].iov_base = (void*)"\n";
                    ioVector[1+i*2+1].iov_len = 1;
                }

                if (writev(STDERR_FILENO, ioVector, numSlots*2+1) == -1)
                {
                    Log::syserror("Failed to dump backtrace to stderr.");
                }
            }
        }

        // let default handler process the signal
        kill(Poco::Process::id(), signal);
    }

    void setFatalSignals()
    {
        struct sigaction action;

        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        action.sa_handler = handleFatalSignal;

        sigaction(SIGSEGV, &action, NULL);
        sigaction(SIGBUS, &action, NULL);
        sigaction(SIGABRT, &action, NULL);
        sigaction(SIGILL, &action, NULL);
        sigaction(SIGFPE, &action, NULL);

        // prepare this in advance just in case.
        std::ostringstream stream;
        stream << "\nFatal signal! Attach debugger with:\n"
               << "sudo gdb --pid=" << Poco::Process::id() << "\n or \n"
               << "sudo gdb --q --n --ex 'thread apply all backtrace full' --batch --pid="
               << Poco::Process::id() << "\n";
        std::string streamStr = stream.str();
        assert (sizeof (FatalGdbString) > strlen(streamStr.c_str()) + 1);
        strncpy(FatalGdbString, streamStr.c_str(), sizeof(FatalGdbString));
    }

    void requestTermination(const Poco::Process::PID& pid)
    {
        try
        {
            Poco::Process::requestTermination(pid);
        }
        catch(const Poco::Exception& exc)
        {
            Log::warn("Util::requestTermination: Exception: " + exc.message());
        }
    }

    int getMemoryUsage(const Poco::Process::PID nPid)
    {
        //TODO: Instead of RSS, return PSS
        const auto cmd = "ps o rss= -p " + std::to_string(nPid);
        FILE* fp = popen(cmd.c_str(), "r");
        if (fp == nullptr)
        {
            return 0;
        }

        std::string sResponse;
        char cmdBuffer[1024];
        while (fgets(cmdBuffer, sizeof(cmdBuffer) - 1, fp) != nullptr)
        {
            sResponse += cmdBuffer;
        }
        pclose(fp);

        int nMem = -1;
        try
        {
            nMem = std::stoi(sResponse);
        }
        catch(std::exception& e)
        {
            Log::warn() << "Trying to find memory of invalid/dead PID" << Log::end;
        }

        return nMem;
    }

    std::string replace(const std::string& s, const std::string& a, const std::string& b)
    {
        std::string result = s;
        std::string::size_type pos;
        while ((pos = result.find(a)) != std::string::npos)
        {
            result = result.replace(pos, a.size(), b);
        }
        return result;
    }

    std::string formatLinesForLog(const std::string& s)
    {
        std::string r;
        std::string::size_type n = s.size();
        if (n > 0 && s.back() == '\n')
            r = s.substr(0, n-1);
        else
            r = s;
        return replace(r, "\n", " / ");
    }

    void setThreadName(const std::string& s)
    {
        if (prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(s.c_str()), 0, 0, 0) != 0)
            Log::syserror("Cannot set thread name to " + s + ".");
    }

    void getVersionInfo(std::string& version, std::string& hash)
    {
        version = std::string(LOOLWSD_VERSION);
        hash = std::string(LOOLWSD_VERSION_HASH);
        hash.resize(std::min(8, (int)hash.length()));
    }

    std::string UniqueId()
    {
        static std::atomic_int counter(0);
        return std::to_string(Poco::Process::id()) + "/" + std::to_string(counter++);
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
