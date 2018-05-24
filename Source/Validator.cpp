/*==============================================================================

  Copyright 2018 by Tracktion Corporation.
  For more information visit www.tracktion.com

   You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   pluginval IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

 ==============================================================================*/

#include "Validator.h"
#include "PluginTests.h"
#include <numeric>

// Defined in Main.cpp, used to create the file logger as early as possible
extern void slaveInitialised();

#if LOG_PIPE_SLAVE_COMMUNICATION
 #define LOG_FROM_MASTER(textToLog) Logger::writeToLog ("*** Recieved:\n" + textToLog);
 #define LOG_TO_MASTER(textToLog)   Logger::writeToLog ("*** Sending:\n" + textToLog);
#else
 #define LOG_FROM_MASTER(textToLog)
 #define LOG_TO_MASTER(textToLog)
#endif

//==============================================================================
struct ForwardingUnitTestRunner : public UnitTestRunner
{
    ForwardingUnitTestRunner (std::function<void (const String&)> fn)
        : callback (std::move (fn))
    {
        jassert (callback);
    }

    void logMessage (const String& message) override
    {
        callback (message);
    }

private:
    std::function<void (const String&)> callback;
};


//==============================================================================
inline Array<UnitTestRunner::TestResult> runTests (PluginTests& test, std::function<void (const String&)> callback)
{
    Array<UnitTestRunner::TestResult> results;
    ForwardingUnitTestRunner testRunner (std::move (callback));
    testRunner.setAssertOnFailure (false);

    Array<UnitTest*> testsToRun;
    testsToRun.add (&test);
    testRunner.runTests (testsToRun);

    for (int i = 0; i < testRunner.getNumResults(); ++i)
        results.add (*testRunner.getResult (i));

    return results;
}

inline Array<UnitTestRunner::TestResult> validate (const PluginDescription& pluginToValidate, int strictnessLevel, std::function<void (const String&)> callback)
{
    PluginTests test (pluginToValidate, strictnessLevel);
    return runTests (test, std::move (callback));
}

inline Array<UnitTestRunner::TestResult> validate (const String& fileOrIDToValidate, int strictnessLevel, std::function<void (const String&)> callback)
{
    PluginTests test (fileOrIDToValidate, strictnessLevel);
    return runTests (test, std::move (callback));
}

inline int getNumFailures (Array<UnitTestRunner::TestResult> results)
{
    return std::accumulate (results.begin(), results.end(), 0,
                            [] (int count, const UnitTestRunner::TestResult& r) { return count + r.failures; });
}

//==============================================================================
namespace IDs
{
    #define DECLARE_ID(name) const Identifier name (#name);

    DECLARE_ID(PLUGINS)
    DECLARE_ID(PLUGIN)
    DECLARE_ID(fileOrID)
    DECLARE_ID(pluginDescription)
    DECLARE_ID(strictnessLevel)

    DECLARE_ID(MESSAGE)
    DECLARE_ID(type)
    DECLARE_ID(text)
    DECLARE_ID(log)
    DECLARE_ID(numFailures)

    #undef DECLARE_ID
}

//==============================================================================
// This is a token that's used at both ends of our parent-child processes, to
// act as a unique token in the command line arguments.
static const char* validatorCommandLineUID = "validatorUID";

// A few quick utility functions to convert between raw data and ValueTrees
static ValueTree memoryBlockToValueTree (const MemoryBlock& mb)
{
    return ValueTree::readFromData (mb.getData(), mb.getSize());
}

static MemoryBlock valueTreeToMemoryBlock (const ValueTree& v)
{
    MemoryOutputStream mo;
    v.writeToStream (mo);

    return mo.getMemoryBlock();
}

static String toXmlString (const ValueTree& v)
{
    if (auto xml = std::unique_ptr<XmlElement> (v.createXml()))
        return xml->createDocument ({}, false, false);

    return {};
}


//==============================================================================
class ValidatorMasterProcess    : public ChildProcessMaster
{
public:
    ValidatorMasterProcess() = default;

    // Callback which can be set to log any calls sent to the slave
    std::function<void (const String&)> logCallback;

    // Callback which can be set to be notified of a lost connection
    std::function<void()> connectionLostCallback;

    //==============================================================================
    // Callback which can be set to be informed when validation starts
    std::function<void (const String&)> validationStartedCallback;

    // Callback which can be set to be informed when a log message is posted
    std::function<void (const String&)> logMessageCallback;

    // Callback which can be set to be informed when a validation completes
    std::function<void (const String&, int)> validationCompleteCallback;

    // Callback which can be set to be informed when all validations have been completed
    std::function<void()> completeCallback;

    //==============================================================================
    Result launch()
    {
        // Make sure we send 0 as the streamFlags args or the pipe can hang during DBG messages
        const bool ok = launchSlaveProcess (File::getSpecialLocation (File::currentExecutableFile),
                                            validatorCommandLineUID, 2000, 0);

        if (! connectionWaiter.wait (5000))
            return Result::fail ("Error: Slave took too long to launch");

        return ok ? Result::ok() : Result::fail ("Error: Slave failed to launch");
    }

    //==============================================================================
    void handleMessageFromSlave (const MemoryBlock& mb) override
    {
        auto v = memoryBlockToValueTree (mb);

        if (v.hasType (IDs::MESSAGE))
        {
            const auto type = v[IDs::type].toString();

            if (logMessageCallback && type == "log")
                logMessageCallback (v[IDs::text].toString());

            if (validationCompleteCallback && type == "result")
                validationCompleteCallback (v[IDs::fileOrID].toString(), v[IDs::numFailures]);

            if (validationStartedCallback && type == "started")
                validationStartedCallback (v[IDs::fileOrID].toString());

            if (completeCallback && type == "complete")
                completeCallback();

            if (type == "connected")
                connectionWaiter.signal();
        }

        logMessage ("Received: " + toXmlString (v));
    }

    // This gets called if the slave process dies.
    void handleConnectionLost() override
    {
        logMessage ("Connection lost to child process!");

        if (connectionLostCallback)
            connectionLostCallback();
    }

    //==============================================================================
    /** Triggers validation of a set of files or IDs. */
    void validate (const StringArray& fileOrIDsToValidate, int strictnessLevel)
    {
        auto v = createPluginsTree (strictnessLevel);

        for (auto fileOrID : fileOrIDsToValidate)
        {
            jassert (fileOrID.isNotEmpty());
            v.appendChild ({ IDs::PLUGIN, {{ IDs::fileOrID, fileOrID }} }, nullptr);
        }

        sendValueTreeToSlave (v);
    }

    /** Triggers validation of a set of PluginDescriptions. */
    void validate (const Array<PluginDescription*>& pluginsToValidate, int strictnessLevel)
    {
        auto v = createPluginsTree (strictnessLevel);

        for (auto pd : pluginsToValidate)
            if (auto xml = std::unique_ptr<XmlElement> (pd->createXml()))
                v.appendChild ({ IDs::PLUGIN, {{ IDs::pluginDescription, Base64::toBase64 (xml->createDocument ("")) }} }, nullptr);

        sendValueTreeToSlave (v);
    }

private:
    WaitableEvent connectionWaiter;

    static ValueTree createPluginsTree (int strictnessLevel)
    {
        ValueTree v (IDs::PLUGINS);
        v.setProperty (IDs::strictnessLevel, strictnessLevel, nullptr);

        return v;
    }

    void sendValueTreeToSlave (const ValueTree& v)
    {
        logMessage ("Sending: " + toXmlString (v));

        if (! sendMessageToSlave (valueTreeToMemoryBlock (v)))
            logMessage ("...failed");
    }

    void logMessage (const String& s)
    {
        if (logCallback)
            logCallback (s);
    }
};

//==============================================================================
Validator::Validator() {}
Validator::~Validator() {}

bool Validator::isConnected() const
{
    return masterProcess != nullptr;
}

bool Validator::validate (const StringArray& fileOrIDsToValidate, int strictnessLevel)
{
    if (! ensureConnection())
        return false;

    masterProcess->validate (fileOrIDsToValidate, strictnessLevel);
    return true;
}

bool Validator::validate (const Array<PluginDescription*>& pluginsToValidate, int strictnessLevel)
{
    if (! ensureConnection())
        return false;

    masterProcess->validate (pluginsToValidate, strictnessLevel);
    return true;
}

//==============================================================================
void Validator::logMessage (const String& m)
{
    listeners.call (&Listener::logMessage, m);
}

bool Validator::ensureConnection()
{
    if (! masterProcess)
    {
        sendChangeMessage();
        masterProcess = std::make_unique<ValidatorMasterProcess>();

       #if LOG_PIPE_COMMUNICATION
        masterProcess->logCallback = [this] (const String& m) { logMessage (m); };
       #endif
        masterProcess->connectionLostCallback = [this]
            {
                listeners.call (&Listener::connectionLost);
                triggerAsyncUpdate();
            };

        masterProcess->validationStartedCallback    = [this] (const String& id) { listeners.call (&Listener::validationStarted, id); };
        masterProcess->logMessageCallback           = [this] (const String& m) { listeners.call (&Listener::logMessage, m); };
        masterProcess->validationCompleteCallback   = [this] (const String& id, int numFailures) { listeners.call (&Listener::itemComplete, id, numFailures); };
        masterProcess->completeCallback             = [this] { listeners.call (&Listener::allItemsComplete); triggerAsyncUpdate(); };

        const auto result = masterProcess->launch();

        if (result.failed())
        {
            logMessage (result.getErrorMessage());
            return false;
        }

        logMessage (String (ProjectInfo::projectName) + " v" + ProjectInfo::versionString
                    + " - " + SystemStats::getJUCEVersion());

        return true;
    }

    return true;
}

void Validator::handleAsyncUpdate()
{
    masterProcess.reset();
    sendChangeMessage();
}

//==============================================================================
/*  This class gets instantiated in the child process, and receives messages from
    the master process.
*/
class ValidatorSlaveProcess : public ChildProcessSlave,
                              private Thread,
                              private DeletedAtShutdown
{
public:
    ValidatorSlaveProcess()
        : Thread ("ValidatorSlaveProcess")
    {
        startThread (4);
    }

    ~ValidatorSlaveProcess()
    {
        stopThread (5000);
    }

    void setConnected (bool isNowConnected) noexcept
    {
        isConnected = isNowConnected;
        sendValueTreeToMaster ({ IDs::MESSAGE, {{ IDs::type, "connected" }} });
    }

    void handleMessageFromMaster (const MemoryBlock& mb) override
    {
        LOG_FROM_MASTER(toXmlString (memoryBlockToValueTree (mb)));
        addRequest (mb);
    }

    void handleConnectionLost() override
    {
        // Force terminate to avoid any zombie processed that can't quit cleanly
        Process::terminate();
    }

private:
    struct LogMessagesSender    : public Thread
    {
        LogMessagesSender (ValidatorSlaveProcess& vsp)
            : Thread ("SlaveMessageSender"), owner (vsp)
        {
            startThread (1);
        }

        ~LogMessagesSender()
        {
            stopThread (2000);
            sendLogMessages();
        }

        void logMessage (const String& m)
        {
            if (! owner.isConnected)
                return;

            const ScopedLock sl (logMessagesLock);
            logMessages.add (m);
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                sendLogMessages();
                Thread::sleep (200);
            }
        }

        void sendLogMessages()
        {
            StringArray messagesToSend;

            {
                const ScopedLock sl (logMessagesLock);
                messagesToSend.swapWith (logMessages);
            }

            if (owner.isConnected && ! messagesToSend.isEmpty())
                owner.sendValueTreeToMaster ({ IDs::MESSAGE, {{ IDs::type, "log" }, { IDs::text, messagesToSend.joinIntoString ("\n") }} });
        }

        ValidatorSlaveProcess& owner;
        CriticalSection logMessagesLock;
        StringArray logMessages;
    };

    CriticalSection requestsLock;
    std::vector<MemoryBlock> requestsToProcess;
    LogMessagesSender logMessagesSender { *this };
    std::atomic<bool> isConnected { false };

    void logMessage (const String& m)
    {
        logMessagesSender.logMessage (m);
    }

    void sendValueTreeToMaster (const ValueTree& v)
    {
        LOG_TO_MASTER(toXmlString (v));
        sendMessageToMaster (valueTreeToMemoryBlock (v));
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            processRequests();

            const ScopedLock sl (requestsLock);

            if (requestsToProcess.empty())
                Thread::sleep (500);
        }
    }

    void addRequest (const MemoryBlock& mb)
    {
        {
            const ScopedLock sl (requestsLock);
            requestsToProcess.push_back (mb);
        }

        notify();
    }

    void processRequests()
    {
        std::vector<MemoryBlock> requests;

        {
            const ScopedLock sl (requestsLock);
            requests.swap (requestsToProcess);
        }

        for (const auto& r : requests)
            processRequest (r);
    }

    void processRequest (MemoryBlock mb)
    {
        const ValueTree v (memoryBlockToValueTree (mb));

        if (v.hasType (IDs::PLUGINS))
        {
            const int strictnessLevel = v.getProperty (IDs::strictnessLevel, 5);

            for (auto c : v)
            {
                String fileOrID;
                Array<UnitTestRunner::TestResult> results;

                if (c.hasProperty (IDs::fileOrID))
                {
                    fileOrID = c[IDs::fileOrID].toString();
                    sendValueTreeToMaster ({
                        IDs::MESSAGE, {{ IDs::type, "started" }, { IDs::fileOrID, fileOrID }}
                    });

                    results = validate (c[IDs::fileOrID].toString(), strictnessLevel, [this] (const String& m) { logMessage (m); });
                }
                else if (c.hasProperty (IDs::pluginDescription))
                {
                    MemoryOutputStream ms;

                    if (Base64::convertFromBase64 (ms, c[IDs::pluginDescription].toString()))
                    {
                        if (auto xml = std::unique_ptr<XmlElement> (XmlDocument::parse (ms.toString())))
                        {
                            PluginDescription pd;

                            if (pd.loadFromXml (*xml))
                            {
                                fileOrID = pd.createIdentifierString();
                                sendValueTreeToMaster ({
                                    IDs::MESSAGE, {{ IDs::type, "started" }, { IDs::fileOrID, fileOrID }}
                                });

                                results = validate (pd, strictnessLevel, [this] (const String& m) { logMessage (m); });
                            }
                        }
                    }
                }

                jassert (fileOrID.isNotEmpty());
                sendValueTreeToMaster ({
                    IDs::MESSAGE, {{ IDs::type, "result" }, { IDs::fileOrID, fileOrID }, { IDs::numFailures, getNumFailures (results) }}
                });
            }
        }

        sendValueTreeToMaster ({
            IDs::MESSAGE, {{ IDs::type, "complete" }}
        });
    }
};

#if JUCE_MAC
static void killWithoutMercy (int)
{
    kill (getpid(), SIGKILL);
}

static void setupSignalHandling()
{
    const int signals[] = { SIGFPE, SIGILL, SIGSEGV, SIGBUS, SIGABRT };

    for (int i = 0; i < numElementsInArray (signals); ++i)
    {
        ::signal (signals[i], killWithoutMercy);
        ::siginterrupt (signals[i], 1);
    }
}
#endif

//==============================================================================
bool invokeSlaveProcessValidator (const String& commandLine)
{
   #if JUCE_MAC
    setupSignalHandling();
   #endif

    auto slave = std::make_unique<ValidatorSlaveProcess>();

    if (slave->initialiseFromCommandLine (commandLine, validatorCommandLineUID))
    {
        slaveInitialised();
        slave->setConnected (true);
        slave.release(); // allow the slave object to stay alive - it'll handle its own deletion.
        return true;
    }

    return false;
}