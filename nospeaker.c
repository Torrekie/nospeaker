// nospeaker · a90.com
//
// Make a Force Touch trackpad talk, sing, and play music by sending short
// waveforms to its haptic motor through the private MultitouchSupport framework.

#include <AudioToolbox/AudioToolbox.h>
#include <Availability.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    double hz, amp;
} Tone;

typedef enum {
    SCENE_INTRO, SCENE_HEART, SCENE_SWEEP, SCENE_TALK, SCENE_SING,
    SCENE_BEAT, SCENE_RAP, SCENE_YOUR_TURN, SCENE_TOUCH, SCENE_SAY, SCENE_OUTRO
} Scene;

typedef struct {
    Scene scene;
    int part;
    const char *partName;
    double hz, level, lastPlay;
    double f0, f1, f2;
    char caption[1024];
    double captionStart, captionDur;
    double kick, snare, hat;
    int step, bar, bars;
    double bpm;
    int noteRow;
    char noteName[16];
    bool touchSeen, touchDown;
    double touchX, touchY, touchEnds, touchWaitUntil;
    bool touchMissed, waitingForKey;
    double followOpenedAt;
    bool inputActive;
    char inputText[281];
    bool inputSubmitted, inputDone;
    const char *sayStatus;
    int sayCount, muted, actuations;
    double started;
    int stepDone, stepTotal;
} LiveState;

static pthread_mutex_t liveLock = PTHREAD_MUTEX_INITIALIZER;
static LiveState state = {
    .scene = SCENE_INTRO, .part = 1, .partName = "LOADING", .lastPlay = -9,
    .captionDur = 1, .kick = -9, .snare = -9, .hat = -9, .step = -1,
    .bars = 4, .bpm = 92, .noteRow = -1, .touchX = 0.5, .touchY = 0.5,
    .followOpenedAt = -9, .sayStatus = "typing", .muted = -1
};

static void lockLive(void) { pthread_mutex_lock(&liveLock); }
static void unlockLive(void) { pthread_mutex_unlock(&liveLock); }

static LiveState snapshot(void)
{
    lockLive();
    LiveState result = state;
    unlockLive();
    return result;
}

static const int partCount = 10;
static atomic_bool stopRequested, skipRequested, keyPressed, jumpToCredits, inCredits;
static double ticksPerSecond;
static uint64_t startTicks;

_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "signal flags require lock-free atomics");

static void requestEnd(void)
{
    if (inCredits || atomic_exchange(&jumpToCredits, true))
        stopRequested = true;
}

static void onSignal(int sig)
{
    if (sig == SIGINT)
        requestEnd();
    else
        stopRequested = true;
}

static bool interrupted(void)
{
    return stopRequested || skipRequested || jumpToCredits;
}

static double nowSeconds(void)
{
    return (double)(mach_absolute_time() - startTicks) / ticksPerSecond;
}

// Sleep most of the way, then spin for at most 1.5 ms to keep the beat steady.
static void waitUntil(uint64_t target)
{
    uint64_t margin = (uint64_t)(0.0015 * ticksPerSecond);
    while (!stopRequested) {
        uint64_t now = mach_absolute_time();
        if (now >= target)
            return;
        if (target - now > margin)
            usleep((useconds_t)((double)(target - now - margin) / ticksPerSecond * 1e6));
    }
}

static void pauseSeconds(double seconds)
{
    double end = nowSeconds() + seconds;
    while (nowSeconds() < end && !interrupted())
        usleep(5000);
}

static void enter(Scene scene, int part, const char *name)
{
    skipRequested = false;
    lockLive();
    state.scene = scene;
    state.part = part;
    state.partName = name;
    unlockLive();
}

static void caption(const char *text, double seconds)
{
    lockLive();
    snprintf(state.caption, sizeof(state.caption), "%s", text);
    state.captionStart = nowSeconds();
    state.captionDur = seconds;
    unlockLive();
}

// Terminal I/O. Keep a duplicate of stdout for the canvas while the private
// framework's own messages are redirected to /dev/null.
static struct termios savedTermios;
static bool terminalTaken;
static int outFD = STDOUT_FILENO;
static int savedOut = -1, savedErr = -1;
static char screenBackground[32] = "\033[48;2;0;0;0m";
static bool lightTheme;

static void out(const char *text)
{
    size_t length = strlen(text);
    while (length) {
        ssize_t count = write(outFD, text, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        text += count;
        length -= (size_t)count;
    }
}

static void terminalSize(int *width, int *height)
{
    struct winsize size;
    if (ioctl(outFD, TIOCGWINSZ, &size) == 0 && size.ws_col && size.ws_row) {
        *width = size.ws_col;
        *height = size.ws_row;
    } else {
        *width = 80;
        *height = 24;
    }
}

static void silenceOthers(void)
{
    fflush(stdout);
    fflush(stderr);
    int nullFD = open("/dev/null", O_WRONLY);
    if (nullFD < 0)
        return;
    savedOut = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    savedErr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    if (savedOut >= 0 && savedErr >= 0) {
        outFD = savedOut;
        dup2(nullFD, STDOUT_FILENO);
        dup2(nullFD, STDERR_FILENO);
    } else {
        if (savedOut >= 0)
            close(savedOut);
        if (savedErr >= 0)
            close(savedErr);
        savedOut = savedErr = -1;
    }
    close(nullFD);
}

static void unsilence(void)
{
    if (savedOut < 0)
        return;
    fflush(stdout);
    fflush(stderr);
    dup2(savedOut, STDOUT_FILENO);
    dup2(savedErr, STDERR_FILENO);
    close(savedOut);
    close(savedErr);
    savedOut = savedErr = -1;
    outFD = STDOUT_FILENO;
}

static void fillScreen(void)
{
    char cursor[32];
    int width, height;
    terminalSize(&width, &height);
    char row[width + 1];
    memset(row, ' ', (size_t)width);
    row[width] = '\0';
    out("\033[0m");
    out(screenBackground);
    out("\033[2J");
    for (int y = 1; y <= height; y++) {
        snprintf(cursor, sizeof(cursor), "\033[%d;1H", y);
        out(cursor);
        out(row);
    }
}

static bool takeTerminal(void)
{
    if (tcgetattr(STDIN_FILENO, &savedTermios) < 0)
        return false;
    struct termios settings = savedTermios;
    settings.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) < 0)
        return false;
    terminalTaken = true;
    silenceOthers();
    out("\033[?1049h\033[?25l\033[?1000h\033[?1002h\033[?1006h");
    fillScreen();
    return true;
}

static void restoreTerminal(void)
{
    if (!terminalTaken)
        return;
    terminalTaken = false;
    out("\033[?1006l\033[?1002l\033[?1000l\033[0m\033[2J\033[?25h\033[?1049l");
    unsilence();
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
}

static void stopRenderer(void);

static _Noreturn void fail(const char *message, const char *headline, int code)
{
    stopRequested = true;
    stopRenderer();
    restoreTerminal();
    fprintf(stderr, "\n  %s\n\n  ", headline ? headline : "nospeaker can't play on this Mac.");
    while (*message) {
        fputc(*message, stderr);
        if (*message++ == '\n')
            fputs("  ", stderr);
    }
    fputs("\n\n", stderr);
    exit(code);
}

static _Noreturn void usageError(const char *message)
{
    const char suffix[] = "\nSee nospeaker --help for the options.";
    size_t length = strlen(message) + sizeof(suffix);
    char *body = malloc(length);
    if (!body)
        fail(message, "nospeaker didn't understand that.", 64);
    snprintf(body, length, "%s%s", message, suffix);
    fail(body, "nospeaker didn't understand that.", 64);
}

// MultitouchSupport types are the calling conventions used by the framework.
// MTActuatorOpen's second argument is zero in Apple's MultitouchHID caller.
typedef int32_t (*ContactCallback)(const void *, const void *, int32_t, double, int32_t, void *);

static void *multitouch;
static CFArrayRef (*createDeviceList)(void);
static int32_t (*getDeviceID)(const void *, uint64_t *);
static bool (*isBuiltIn)(const void *);
static CFTypeRef (*actuatorCreate)(uint64_t);
static IOReturn (*actuatorOpen)(CFTypeRef, uint32_t);
static IOReturn (*actuatorClose)(CFTypeRef);
static CFTypeRef (*actuationCreate)(CFDictionaryRef, int32_t);
static IOReturn (*actuationPlay)(CFTypeRef, CFTypeRef, uint32_t, float, float);
static bool (*registerCallback)(const void *, ContactCallback);
static bool (*unregisterCallback)(const void *, ContactCallback);
static IOReturn (*deviceStart)(const void *, int32_t);
static IOReturn (*deviceStop)(const void *);

static CFArrayRef deviceList;
static const void *motorDevice;
static CFTypeRef motor;
static int deviceCount;
static bool motorBuiltIn;

static mach_port_t defaultIOKitPort(void)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
    if (__builtin_available(macOS 12.0, *))
        return kIOMainPortDefault;
#endif
    return MACH_PORT_NULL; // IOKit selects the default port for a null argument.
}

static bool isLaptop(void)
{
    char model[64];
    size_t size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, NULL, 0) == 0 && strncmp(model, "MacBook", 7) == 0)
        return true;
    io_service_t battery = IOServiceGetMatchingService(defaultIOKitPort(), IOServiceMatching("AppleSmartBattery"));
    if (!battery)
        return false;
    IOObjectRelease(battery);
    return true;
}

static _Noreturn void noTrackpad(bool found)
{
    if (!found && !isLaptop()) {
        fail("This Mac doesn't have a built-in trackpad, and no trackpad is connected.\n"
             "Connect a Magic Trackpad (2 or newer, the ones with Force Touch) and run it again.", NULL, 1);
    }
    if (!found)
        fail("No trackpad was found. Make sure the trackpad is on and connected, then run it again.", NULL, 1);
    fail("Found a trackpad, but couldn't open its Force Touch actuator.\n"
         "It needs a compatible Mac with Force Touch, or a Magic Trackpad 2 or newer.", NULL, 1);
}

static void openMotor(void)
{
    multitouch = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport",
                       RTLD_NOW | RTLD_LOCAL);
    if (!multitouch) {
        multitouch = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/Versions/A/MultitouchSupport",
                           RTLD_NOW | RTLD_LOCAL);
    }
    if (!multitouch)
        fail("This version of macOS couldn't load MultitouchSupport.", NULL, 1);

    createDeviceList = dlsym(multitouch, "MTDeviceCreateList");
    getDeviceID = dlsym(multitouch, "MTDeviceGetDeviceID");
    isBuiltIn = dlsym(multitouch, "MTDeviceIsBuiltIn");
    actuatorCreate = dlsym(multitouch, "MTActuatorCreateFromDeviceID");
    actuatorOpen = dlsym(multitouch, "MTActuatorOpen");
    actuatorClose = dlsym(multitouch, "MTActuatorClose");
    actuationCreate = dlsym(multitouch, "MTActuationCreateFromDictionary");
    actuationPlay = dlsym(multitouch, "MTActuationActuate");
    registerCallback = dlsym(multitouch, "MTRegisterContactFrameCallback");
    unregisterCallback = dlsym(multitouch, "MTUnregisterContactFrameCallback");
    deviceStart = dlsym(multitouch, "MTDeviceStart");
    deviceStop = dlsym(multitouch, "MTDeviceStop");

    if (!createDeviceList || !getDeviceID || !actuatorCreate || !actuatorOpen ||
        !actuatorClose || !actuationCreate || !actuationPlay)
        fail("This version of macOS doesn't have the trackpad functions this needs.", NULL, 1);

    deviceList = createDeviceList();
    if (!deviceList || CFArrayGetCount(deviceList) == 0)
        noTrackpad(false);

    typedef struct {
        const void *device;
        uint64_t id;
        bool builtIn;
    } Device;
    CFIndex count = CFArrayGetCount(deviceList);
    Device *devices = calloc((size_t)count, sizeof(*devices));
    if (!devices)
        fail("It couldn't allocate the trackpad list.", NULL, 1);
    for (CFIndex i = 0; i < count; i++) {
        const void *device = CFArrayGetValueAtIndex(deviceList, i);
        uint64_t id;
        if (getDeviceID(device, &id) == 0)
            devices[deviceCount++] = (Device){device, id, isBuiltIn && isBuiltIn(device)};
    }

    // Prefer the built-in motor and keep the framework's order within each group.
    for (int builtIn = 1; builtIn >= 0 && !motor; builtIn--) {
        for (int i = 0; i < deviceCount; i++) {
            Device device = devices[i];
            if (device.builtIn != builtIn)
                continue;
            CFTypeRef candidate = actuatorCreate(device.id);
            if (!candidate)
                continue;
            if (actuatorOpen(candidate, 0) == 0) {
                motor = candidate;
                motorDevice = device.device;
                motorBuiltIn = device.builtIn;
                break;
            }
            CFRelease(candidate);
        }
    }
    free(devices);
    if (!motor)
        noTrackpad(true);
}

// Each tone's strength is normalized before it is given to the driver.
static void play(const Tone *tones, size_t count, double ms, float level)
{
    if (!motor || !count)
        return;

    double sum = 0, zero = 0;
    for (size_t i = 0; i < count; i++)
        sum += tones[i].amp;
    sum = fmax(sum, 0.0001);

    CFNumberRef zeroNumber = CFNumberCreate(NULL, kCFNumberDoubleType, &zero);
    CFNumberRef duration = CFNumberCreate(NULL, kCFNumberDoubleType, &ms);
    const void *baseKeys[] = {CFSTR("Type"), CFSTR("Amplitude"), CFSTR("DurationMS")};
    const void *baseValues[] = {CFSTR("Gaussian"), zeroNumber, duration};
    CFDictionaryRef base = CFDictionaryCreate(NULL, baseKeys, baseValues, 3,
                                             &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef list = CFArrayCreateMutable(NULL, (CFIndex)count, &kCFTypeArrayCallBacks);
    const void *toneKeys[] = {CFSTR("Type"), CFSTR("Amplitude"), CFSTR("DurationMS"), CFSTR("DelayMS"), CFSTR("FrequencykHz")};
    for (size_t i = 0; i < count; i++) {
        double frequency = tones[i].hz / 1000;
        double strength = tones[i].amp / sum;
        CFNumberRef frequencyNumber = CFNumberCreate(NULL, kCFNumberDoubleType, &frequency);
        CFNumberRef strengthNumber = CFNumberCreate(NULL, kCFNumberDoubleType, &strength);
        const void *values[] = {CFSTR("Sine"), strengthNumber, duration, zeroNumber, frequencyNumber};
        CFDictionaryRef tone = CFDictionaryCreate(NULL, toneKeys, values, 5,
                                                 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayAppendValue(list, tone);
        CFRelease(tone);
        CFRelease(strengthNumber);
        CFRelease(frequencyNumber);
    }
    const void *keys[] = {CFSTR("BaseWaveform"), CFSTR("Tones")};
    const void *values[] = {base, list};
    CFDictionaryRef shape = CFDictionaryCreate(NULL, keys, values, 2,
                                              &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef actuation = actuationCreate(shape, 0);
    CFRelease(shape);
    CFRelease(list);
    CFRelease(base);
    CFRelease(duration);
    CFRelease(zeroNumber);
    if (!actuation)
        return;

    (void)actuationPlay(actuation, motor, 0, fminf(1, fmaxf(0, level)), 0);
    CFRelease(actuation);
    lockLive();
    state.actuations++;
    state.hz = tones[0].hz;
    state.level = level;
    state.lastPlay = nowSeconds();
    unlockLive();
}

// Return -1 when the default audio output doesn't expose its mute or volume.
static int speakersMuted(void)
{
    // Main and its old SDK spelling Master designate the same element.
    const AudioObjectPropertyElement mainElement = 0;
    AudioDeviceID device = 0;
    UInt32 size = sizeof(device);
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, mainElement
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr)
        return -1;

    UInt32 muted = 0;
    size = sizeof(muted);
    address = (AudioObjectPropertyAddress){kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput, mainElement};
    bool known = AudioObjectHasProperty(device, &address) &&
                 AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &muted) == noErr;
    if (known && muted)
        return 1;

    address.mSelector = kAudioDevicePropertyVolumeScalar;
    for (AudioObjectPropertyElement element = mainElement; element <= 1; element++) {
        Float32 volume = 1;
        size = sizeof(volume);
        address.mElement = element;
        if (AudioObjectHasProperty(device, &address) &&
            AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &volume) == noErr)
            return volume < 0.001f;
    }
    return known ? 0 : -1;
}

typedef struct {
    bool down;
    float x, y, size;
} Contact;

static pthread_mutex_t touchLock = PTHREAD_MUTEX_INITIALIZER;
static Contact currentContact;
static bool touchStarted;

static Contact getContact(void)
{
    pthread_mutex_lock(&touchLock);
    Contact contact = currentContact;
    pthread_mutex_unlock(&touchLock);
    return contact;
}

static int32_t touchCallback(const void *device, const void *data, int32_t count,
                             double timestamp, int32_t frame, void *refcon)
{
    (void)device;
    (void)timestamp;
    (void)frame;
    (void)refcon;
    Contact contact = {0};
    if (data) {
        // The private callback uses 96-byte records on the supported layout.
        for (int32_t i = 0; i < count; i++) {
            const char *record = (const char *)data + (size_t)i * 96;
            int32_t phase;
            memcpy(&phase, record + 20, sizeof(phase));
            if (phase < 3 || phase > 5)
                continue;
            contact.down = true;
            memcpy(&contact.x, record + 32, sizeof(contact.x));
            memcpy(&contact.y, record + 36, sizeof(contact.y));
            memcpy(&contact.size, record + 48, sizeof(contact.size));
        }
    }
    pthread_mutex_lock(&touchLock);
    currentContact = contact;
    pthread_mutex_unlock(&touchLock);
    return 0;
}

static bool startTouch(void)
{
    if (!registerCallback || !unregisterCallback || !deviceStart || !deviceStop)
        return false;

    bool quiet = !terminalTaken;
    if (quiet)
        silenceOthers();
    (void)registerCallback(motorDevice, touchCallback);
    touchStarted = deviceStart(motorDevice, 0) == 0;
    if (!touchStarted)
        (void)unregisterCallback(motorDevice, touchCallback);
    if (quiet) {
        usleep(200000);
        unsilence();
    }
    return touchStarted;
}

static void stopTouch(void)
{
    if (!touchStarted)
        return;
    (void)unregisterCallback(motorDevice, touchCallback);
    (void)deviceStop(motorDevice);
    touchStarted = false;
}

static void closeMotor(void)
{
    stopTouch();
    if (motor) {
        (void)actuatorClose(motor);
        CFRelease(motor);
    }
    if (deviceList)
        CFRelease(deviceList);
    // Keep the private framework mapped until its contact callbacks have exited.
}
// Speech synthesis and analysis.

typedef struct {
    double formants[3], amps[3];
    float level;
    bool voiced;
    double f0;
} Frame;

typedef struct {
    Frame *items;
    size_t count;
} Frames;

enum { SAMPLE_RATE = 16000, WINDOW = 400, HOP_SAMPLES = 160, LPC_ORDER = 18 };

static const double hop = 0.010;
static const char *voiceName;

extern char **environ;

static int waitForChild(pid_t child)
{
    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    return result == child ? status : -1;
}

static CFMutableStringRef lowercaseUTF8(const char *text, size_t size)
{
    CFStringRef string = CFStringCreateWithBytes(NULL, (const UInt8 *)text,
                                                (CFIndex)size, kCFStringEncodingUTF8, false);
    if (!string)
        return NULL;
    CFMutableStringRef lower = CFStringCreateMutableCopy(NULL, 0, string);
    CFRelease(string);
    if (lower)
        CFStringLowercase(lower, NULL);
    return lower;
}

// An unknown voice name may make say fall back to its default voice.
static bool voiceExists(const char *name)
{
    int voicePipe[2];
    if (pipe(voicePipe) == -1)
        return true;

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    pid_t child;
    if (error == 0) {
        error = posix_spawn_file_actions_addclose(&actions, voicePipe[0]);
        if (error == 0)
            error = posix_spawn_file_actions_adddup2(&actions, voicePipe[1], STDOUT_FILENO);
        if (error == 0 && voicePipe[1] != STDOUT_FILENO)
            error = posix_spawn_file_actions_addclose(&actions, voicePipe[1]);
        if (error == 0) {
            char *argv[] = {"/usr/bin/say", "-v", "?", NULL};
            error = posix_spawn(&child, argv[0], &actions, NULL, argv, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    close(voicePipe[1]);
    if (error != 0) {
        close(voicePipe[0]);
        return true;
    }

    FILE *output = fdopen(voicePipe[0], "r");
    if (!output) {
        close(voicePipe[0]);
        waitForChild(child);
        return true;
    }

    CFMutableStringRef wanted = lowercaseUTF8(name, strlen(name));
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool valid = wanted != NULL;
    bool found = false;
    while ((length = getline(&line, &capacity, output)) != -1) {
        CFMutableStringRef entry = lowercaseUTF8(line, (size_t)length);
        if (entry) {
            if (wanted && CFStringHasPrefix(entry, wanted)) {
                CFIndex prefixLength = CFStringGetLength(wanted);
                if (prefixLength < CFStringGetLength(entry)) {
                    UniChar separator = CFStringGetCharacterAtIndex(entry, prefixLength);
                    found |= separator == ' ' || separator == '\t';
                }
            }
            CFRelease(entry);
        } else {
            valid = false;
        }
    }
    free(line);
    if (wanted)
        CFRelease(wanted);
    fclose(output);
    waitForChild(child);
    return valid && found;
}

static void badVoice(const char *name, int code)
{
    char *message;
    if (asprintf(&message, "There's no voice called \"%s\" on this Mac.\n"
                           "List the voices you have with: say -v '?'", name) >= 0)
        fail(message, "nospeaker can't use that voice.", code);
    fail("List the voices you have with: say -v '?'", "nospeaker can't use that voice.", code);
}

static void speechFailure(void)
{
    if (voiceName)
        badVoice(voiceName, 1);
    fail("macOS's voice (say) couldn't speak that line.", "nospeaker couldn't prepare its voice.", 1);
}

static float *readSpeechFile(const char *path, size_t *sampleCount)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), false);
    if (!url)
        return NULL;

    ExtAudioFileRef file;
    OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (status != noErr)
        return NULL;

    SInt64 frameLength;
    UInt32 propertySize = sizeof(frameLength);
    status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames,
                                     &propertySize, &frameLength);
    AudioStreamBasicDescription source;
    if (status == noErr) {
        propertySize = sizeof(source);
        status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat,
                                         &propertySize, &source);
    }
    if (status != noErr || frameLength < 0 ||
        source.mChannelsPerFrame == 0 || source.mChannelsPerFrame > UINT32_MAX / sizeof(float)) {
        ExtAudioFileDispose(file);
        return NULL;
    }

    UInt32 channels = source.mChannelsPerFrame;
    UInt32 bytesPerFrame = channels * sizeof(float);
    if ((UInt64)frameLength > UINT32_MAX / bytesPerFrame) {
        ExtAudioFileDispose(file);
        return NULL;
    }

    AudioStreamBasicDescription format = {
        .mSampleRate = source.mSampleRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = bytesPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = bytesPerFrame,
        .mChannelsPerFrame = channels,
        .mBitsPerChannel = 8 * sizeof(float),
    };
    status = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(format), &format);
    UInt32 byteCount = (UInt32)frameLength * bytesPerFrame;
    float *samples = status == noErr ? malloc(byteCount ? byteCount : sizeof(float)) : NULL;
    if (!samples) {
        ExtAudioFileDispose(file);
        return NULL;
    }
    AudioBufferList buffers = {
        .mNumberBuffers = 1,
        .mBuffers = {{.mNumberChannels = channels, .mDataByteSize = byteCount, .mData = samples}},
    };
    UInt32 framesRead = (UInt32)frameLength;
    if (framesRead)
        status = ExtAudioFileRead(file, &framesRead, &buffers);
    ExtAudioFileDispose(file);
    if (status != noErr) {
        free(samples);
        return NULL;
    }

    // Swift reads floatChannelData[0]. Keep the first channel for analysis.
    if (channels > 1) {
        for (UInt32 i = 0; i < framesRead; ++i)
            samples[i] = samples[(size_t)i * channels];
    }
    *sampleCount = framesRead;
    return samples;
}

static double lpc(const double x[WINDOW], double a[LPC_ORDER + 1])
{
    double correlations[LPC_ORDER + 1];
    for (int lag = 0; lag <= LPC_ORDER; ++lag) {
        double sum = 0;
        for (int i = lag; i < WINDOW; ++i)
            sum += x[i] * x[i - lag];
        correlations[lag] = sum;
    }
    a[0] = 1;
    if (correlations[0] <= 1e-12)
        return 0;

    correlations[0] *= 1.0001;
    double error = correlations[0];
    for (int i = 1; i <= LPC_ORDER; ++i) {
        double acc = correlations[i];
        for (int j = 1; j < i; ++j)
            acc += a[j] * correlations[i - j];
        double k = -acc / error;
        double next[LPC_ORDER + 1];
        for (int j = 1; j < i; ++j)
            next[j] = a[j] + k * a[i - j];
        for (int j = 1; j < i; ++j)
            a[j] = next[j];
        a[i] = k;
        error *= 1 - k * k;
    }
    return error;
}

typedef struct {
    double re, im;
} Complex;

static Complex multiplyComplex(Complex a, Complex b)
{
    return (Complex){a.re * b.re - a.im * b.im, a.im * b.re + a.re * b.im};
}

static void roots(const double a[LPC_ORDER + 1], Complex z[LPC_ORDER])
{
    for (int k = 0; k < LPC_ORDER; ++k) {
        double angle = 2 * M_PI * k / LPC_ORDER + 0.4;
        z[k] = (Complex){0.9 * cos(angle), 0.9 * sin(angle)};
    }
    for (int iteration = 0; iteration < 300; ++iteration) {
        double moved = 0;
        for (int i = 0; i < LPC_ORDER; ++i) {
            Complex d = {1, 0};
            for (int j = 0; j < LPC_ORDER; ++j) {
                if (i != j)
                    d = multiplyComplex(d, (Complex){z[i].re - z[j].re, z[i].im - z[j].im});
            }
            if (d.re == 0 && d.im == 0)
                d.re = 1e-12;

            Complex value = {1, 0};
            for (int k = 1; k <= LPC_ORDER; ++k) {
                value = multiplyComplex(value, z[i]);
                value.re += a[k];
            }
            double divisor = d.re * d.re + d.im * d.im;
            Complex step = {
                (value.re * d.re + value.im * d.im) / divisor,
                (value.im * d.re - value.re * d.im) / divisor,
            };
            z[i].re -= step.re;
            z[i].im -= step.im;
            moved = fmax(moved, fabs(step.re) + fabs(step.im));
        }
        if (moved < 1e-10)
            break;
    }
}

static double pickFormant(const double *candidates, size_t count, double lo, double hi,
                          double near)
{
    double selected = near;
    double best = INFINITY;
    for (size_t i = 0; i < count; ++i) {
        double hz = candidates[i];
        if (hz >= lo && hz <= hi && fabs(hz - near) < best) {
            selected = hz;
            best = fabs(hz - near);
        }
    }
    return selected;
}

// Upper median of a window of at most five frames, including both ends.
static double median(double *values, size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        double value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
    return values[count / 2];
}

static Frames analyse(const float *samples, size_t sampleCount)
{
    if (sampleCount < WINDOW)
        return (Frames){0};
    size_t count = 1 + (sampleCount - WINDOW) / HOP_SAMPLES;
    Frame *frames = calloc(count, sizeof(*frames));
    Frame *raw = calloc(count, sizeof(*raw));
    double *rms = calloc(count, sizeof(*rms));
    if (!frames || !raw || !rms) {
        free(frames);
        free(raw);
        free(rms);
        fail("There isn't enough memory to analyse the speech.",
             "nospeaker couldn't prepare its voice.", 1);
    }

    double hamming[WINDOW];
    for (int i = 0; i < WINDOW; ++i)
        hamming[i] = 0.54 - 0.46 * cos(2 * M_PI * i / (WINDOW - 1));

    Frame previous = {
        .formants = {500, 1500, 2500},
        .amps = {1, 0.5, 0.25},
        .f0 = 110,
    };
    double peak = 0;
    for (size_t frameIndex = 0; frameIndex < count; ++frameIndex) {
        const float *input = samples + frameIndex * HOP_SAMPLES;
        double seg[WINDOW], filtered[WINDOW], energy = 0;
        int crossings = 0;
        for (int i = 0; i < WINDOW; ++i) {
            seg[i] = input[i];
            energy += seg[i] * seg[i];
            double before = i > 0 ? seg[i - 1] : 0;
            crossings += i > 0 && (seg[i] >= 0) != (before >= 0);
            filtered[i] = (seg[i] - 0.97 * before) * hamming[i];
        }
        rms[frameIndex] = sqrt(energy / WINDOW);
        peak = fmax(peak, rms[frameIndex]);

        int bestLag = 0;
        double bestR = 0;
        for (int lag = SAMPLE_RATE / 300; lag <= SAMPLE_RATE / 70 && lag < WINDOW - 40; ++lag) {
            double product = 0, ea = 0, eb = 0;
            for (int i = 0; i < WINDOW - lag; ++i) {
                double a = seg[i], b = seg[i + lag];
                product += a * b;
                ea += a * a;
                eb += b * b;
            }
            double r = product / fmax(sqrt(ea * eb), 1e-12);
            if (r > bestR) {
                bestR = r;
                bestLag = lag;
            }
        }

        Frame f = previous;
        f.voiced = bestR > 0.35 && (double)crossings / WINDOW < 0.22;
        if (f.voiced && bestLag > 0)
            f.f0 = (double)SAMPLE_RATE / bestLag;

        double coefficients[LPC_ORDER + 1];
        if (lpc(filtered, coefficients) > 0) {
            Complex z[LPC_ORDER];
            roots(coefficients, z);
            double candidates[LPC_ORDER];
            size_t candidateCount = 0;
            for (int i = 0; i < LPC_ORDER; ++i) {
                if (z[i].im > 0) {
                    double hz = atan2(z[i].im, z[i].re) * SAMPLE_RATE / (2 * M_PI);
                    double radius = sqrt(z[i].re * z[i].re + z[i].im * z[i].im);
                    double bandwidth = -log(fmax(1e-9, radius)) * SAMPLE_RATE / M_PI;
                    if (hz > 150 && hz < 4200 && bandwidth < 500)
                        candidates[candidateCount++] = hz;
                }
            }
            f.formants[0] = pickFormant(candidates, candidateCount, 200, 1000, previous.formants[0]);
            f.formants[1] = pickFormant(candidates, candidateCount,
                                        fmax(f.formants[0] + 200, 650), 2700, previous.formants[1]);
            f.formants[2] = pickFormant(candidates, candidateCount,
                                        fmax(f.formants[1] + 300, 1700), 3900, previous.formants[2]);
            for (int j = 0; j < 3; ++j) {
                double w = 2 * M_PI * f.formants[j] / SAMPLE_RATE;
                double re = 0, im = 0;
                for (int k = 0; k <= LPC_ORDER; ++k) {
                    re += coefficients[k] * cos(w * k);
                    im -= coefficients[k] * sin(w * k);
                }
                f.amps[j] = 1 / fmax(sqrt(re * re + im * im), 1e-6);
            }
        }
        raw[frameIndex] = f;
        previous = f;
    }

    for (size_t i = 0; i < count; ++i) {
        Frame f = raw[i];
        double relative = rms[i] / fmax(peak, 1e-9);
        f.level = relative < 0.02 ? 0 : (float)fmin(1, pow(relative, 0.45));
        if (f.level == 0)
            f.voiced = false;

        size_t lo = i > 2 ? i - 2 : 0;
        size_t hi = i + 2 < count ? i + 2 : count - 1;
        double values[5];
        for (int j = 0; j < 3; ++j) {
            for (size_t k = lo; k <= hi; ++k)
                values[k - lo] = raw[k].formants[j];
            f.formants[j] = median(values, hi - lo + 1);
        }
        for (size_t k = lo; k <= hi; ++k)
            values[k - lo] = raw[k].f0;
        f.f0 = fmin(250, fmax(70, median(values, hi - lo + 1)));
        frames[i] = f;
    }
    free(raw);
    free(rms);
    return (Frames){frames, count};
}

static Frames render(const char *text)
{
    const char *temporaryRoot = getenv("TMPDIR");
    if (!temporaryRoot || !*temporaryRoot)
        temporaryRoot = "/tmp";
    char directory[PATH_MAX];
    char path[PATH_MAX];
    int length = snprintf(directory, sizeof(directory), "%s/nospeaker.XXXXXX", temporaryRoot);
    if (length < 0 || (size_t)length >= sizeof(directory) || !mkdtemp(directory))
        speechFailure();
    length = snprintf(path, sizeof(path), "%s/voice.wav", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        rmdir(directory);
        speechFailure();
    }

    char *argv[9] = {"/usr/bin/say", "-o", path, "--file-format=WAVE", "--data-format=LEF32@16000"};
    int argument = 5;
    if (voiceName) {
        argv[argument++] = "-v";
        argv[argument++] = (char *)voiceName;
    }
    argv[argument] = (char *)text;
    pid_t child;
    if (posix_spawn(&child, argv[0], NULL, NULL, argv, environ) != 0) {
        rmdir(directory);
        fail("It couldn't run macOS's voice (/usr/bin/say), which it needs to talk.", NULL, 1);
    }

    int status = waitForChild(child);
    size_t sampleCount;
    float *samples = status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0
        ? readSpeechFile(path, &sampleCount) : NULL;
    unlink(path);
    rmdir(directory);
    if (!samples)
        speechFailure();

    Frames result = analyse(samples, sampleCount);
    free(samples);
    size_t first = 0;
    while (first < result.count && result.items[first].level == 0)
        ++first;
    while (result.count > first && result.items[result.count - 1].level == 0)
        --result.count;
    result.count -= first;
    if (first && result.count)
        memmove(result.items, result.items + first, result.count * sizeof(*result.items));
    return result;
}

static void voiceTones(Frame f, Tone tones[2])
{
    double secondWeight = f.amps[1] * 1.8;
    double sum = fmax(f.amps[0] + secondWeight, 1e-9);
    tones[0] = (Tone){f.formants[0], f.amps[0] / sum};
    tones[1] = (Tone){f.formants[1], secondWeight / sum};
}

/* Musical scenes */

static const char *const noteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};
static const char *const rollNotes[] = {"C7", "A6", "G6", "E6", "C6", "A5"};

static double hz(const char *note)
{
    size_t length = strlen(note);
    int octave = note[length - 1] - '0';
    for (int semitone = 0; semitone < 12; ++semitone) {
        const char *name = noteNames[semitone];
        if (strlen(name) == length - 1 && strncmp(note, name, length - 1) == 0)
            return 440 * pow(2, (semitone - 9 + (octave - 4) * 12) / 12.0);
    }
    return 440;
}

static void noteName(double frequency, char name[16])
{
    int midi = (int)round(69 + 12 * log2(frequency / 440));
    snprintf(name, 16, "%s%d", noteNames[(midi % 12 + 12) % 12], midi / 12 - 1);
}

static double randomBetween(double lower, double upper)
{
    uint64_t bits;
    arc4random_buf(&bits, sizeof(bits));
    return lower + (upper - lower) * ((double)(bits >> 11) / 9007199254740991.0);
}

static void thump(double amplitude)
{
    const uint64_t start = mach_absolute_time();
    for (double t = 0; t < 0.11 && !stopRequested;) {
        const double f = 120 + 780 * exp(-t / 0.02);
        const double segment = 1 / f;
        play(&(Tone){f, 1}, 1, segment * 1000, (float)(amplitude * exp(-t / 0.05)));
        t += segment;
        waitUntil(start + (uint64_t)(t * ticksPerSecond));
    }
}

static void heartbeat(void)
{
    for (int i = 0; i < 2 && !interrupted(); ++i) {
        thump(1);
        pauseSeconds(0.17);
        thump(0.7);
        pauseSeconds(0.75);
    }
}

static void sweep(double seconds)
{
    const uint64_t start = mach_absolute_time();
    for (double t = 0; t < seconds && !interrupted();) {
        const double f = 100 * pow(40, t / seconds);
        const double segment = fmax(1, round(0.02 * f)) / f;
        play(&(Tone){f, 1}, 1, segment * 1000, 1);
        t += segment;
        waitUntil(start + (uint64_t)(t * ticksPerSecond));
    }
}

static void talk(Frames frames, const char *text)
{
    if (frames.count == 0)
        return;

    const double total = frames.count * hop;
    caption(text, total);
    const uint64_t start = mach_absolute_time();
    for (double t = 0; t < total && !interrupted();) {
        size_t index = (size_t)(t / hop);
        Frame frame = frames.items[index < frames.count ? index : frames.count - 1];
        double step = 0.005;
        if (frame.level > 0) {
            if (frame.voiced) {
                Tone tones[2];
                step = 1 / frame.f0;
                voiceTones(frame, tones);
                play(tones, 2, step * 1000, frame.level);
                lockLive();
                state.f0 = frame.f0;
                state.f1 = frame.formants[0];
                state.f2 = frame.formants[1];
                unlockLive();
            } else {
                Tone tones[3];
                step = 0.004;
                for (size_t i = 0; i < 3; ++i)
                    tones[i] = (Tone){randomBetween(2700, 4300), 1};
                play(tones, 3, step * 1000, frame.level * 0.7f);
            }
        }
        t += step;
        waitUntil(start + (uint64_t)(t * ticksPerSecond));
    }
    lockLive();
    state.f0 = 0;
    state.f1 = 0;
    state.f2 = 0;
    unlockLive();
}

/* The hook glides between notes and scoops up from below at the start of each phrase. */
static void sing(void)
{
    static const struct { const char *name; double beats; } melody[] = {
        {"E6", 0.5}, {"G6", 0.5}, {"A6", 1},   {"G6", 0.5}, {"E6", 0.5}, {"C6", 1},
        {"E6", 0.5}, {"G6", 0.5}, {"A6", 0.5}, {"C7", 0.5}, {"A6", 1},   {"G6", 1},
        {"E6", 0.5}, {"C6", 0.5}, {"A5", 1},   {"C6", 0.5}, {"E6", 0.5}, {"A6", 2.5},
    };
    const double beat = 60.0 / 112;
    const uint64_t start = mach_absolute_time();
    double previous = 0;
    double t = 0;
    double at = 0;
    for (size_t i = 0; i < sizeof(melody) / sizeof(*melody) && !interrupted(); ++i) {
        const char *name = melody[i].name;
        const double target = hz(name);
        const double length = melody[i].beats * beat;
        const bool phraseEnd = i == 5 || i == 11 || i == 17;
        const double sounding = phraseEnd ? length * 0.86 : length;
        int row = -1;
        for (size_t j = 0; j < sizeof(rollNotes) / sizeof(*rollNotes); ++j) {
            if (strcmp(name, rollNotes[j]) == 0) {
                row = (int)j;
                break;
            }
        }
        lockLive();
        state.noteRow = row;
        snprintf(state.noteName, sizeof(state.noteName), "%s", name);
        unlockLive();

        while (t < at + sounding && !interrupted()) {
            const double age = t - at;
            double f = target;
            if (previous > 0 && age < 0.045)
                f = previous * pow(target / previous, age / 0.045);
            else if (previous == 0 && age < 0.04)
                f = target * pow(2, -0.6 * (1 - age / 0.04) / 12);
            if (age > 0.2)
                f *= pow(2, fmin(1, (age - 0.2) / 0.25) * 0.22 * sin(2 * M_PI * 5.8 * age) / 12);

            double level = age < 0.015 ? 0.6 + 0.4 * age / 0.015 : 1;
            if (i == sizeof(melody) / sizeof(*melody) - 1)
                level *= fmax(0, 1 - age / length);
            const double segment = fmax(1, round((age < 0.06 ? 0.02 : 0.045) * f)) / f;
            play(&(Tone){f, 1}, 1, fmin(segment, at + sounding - t + 0.004) * 1000, (float)level);
            t += segment;
            waitUntil(start + (uint64_t)(t * ticksPerSecond));
        }
        previous = phraseEnd ? 0 : target;
        lockLive();
        state.noteRow = -1;
        unlockLive();
        at += length;
        if (t < at) {
            t = at;
            waitUntil(start + (uint64_t)(t * ticksPerSecond));
        }
    }
}

/* Mix drums and bass until the voice takes over the carrier and ducks the drums. */
static void groove(int bars, int rapBar, Frames rap, const char *rapText)
{
    const double bpm = 92;
    const double sixteenth = 15 / bpm;
    const double barLength = 16 * sixteenth;
    static const char *const patterns[] = {
        "X.........X..x..", "....X.......X...", "x.x.x.x.x.x.xxx.",
    };
    static const char bassRhythm[] = "x.....x...x.....";
    static const double bassHz[] = {110, 87.31, 130.81, 98};
    static const double drumDuck[] = {0.4, 0.25, 0};

    struct Hit { double time; int kind; double accent; };
    struct Hit hits[bars * 16 * 3];
    size_t hitCount = 0;
    for (int step = 0; step < bars * 16; ++step) {
        for (int kind = 0; kind < 3; ++kind) {
            char mark = patterns[kind][step % 16];
            if (mark != '.')
                hits[hitCount++] = (struct Hit){step * sixteenth, kind, mark == 'X' ? 1 : 0.65};
        }
    }
    lockLive();
    state.bpm = bpm;
    state.bars = bars;
    unlockLive();

    const double rapStart = rapBar * barLength;
    const double end = bars * barLength;
    const uint64_t start = mach_absolute_time();
    size_t nextHit = 0;
    size_t firstRinging = 0;
    bool rapping = false;
    for (double t = 0; t < end && !stopRequested;) {
        if ((skipRequested && !rapping) || jumpToCredits)
            break;
        while (nextHit < hitCount && hits[nextHit].time <= t + 0.0005) {
            const struct Hit hit = hits[nextHit++];
            const double when = nowSeconds();
            lockLive();
            if (hit.kind == 0)
                state.kick = when;
            else if (hit.kind == 1)
                state.snare = when;
            else
                state.hat = when;
            unlockLive();
        }
        while (firstRinging < nextHit && t - hits[firstRinging].time > 0.2)
            ++firstRinging;

        const int i16 = (int)(t / sixteenth);
        lockLive();
        state.step = i16 % 16;
        state.bar = i16 / 16;
        unlockLive();
        if (!rapping && t >= rapStart) {
            rapping = true;
            enter(SCENE_RAP, 6, "RAP");
            caption(rapText, rap.count * hop);
        }

        const Frame *frame = NULL;
        if (t >= rapStart) {
            size_t index = (size_t)((t - rapStart) / hop);
            if (index < rap.count)
                frame = &rap.items[index];
        }
        const bool talking = frame && frame->level > 0;
        const double bass = bassHz[(i16 / 16) % (sizeof(bassHz) / sizeof(*bassHz))];
        const double carrier = talking && frame->voiced ? frame->f0 : bass;
        double step = 1 / carrier;
        Tone tones[2 + 2 * (nextHit - firstRinging)];
        size_t toneCount = 2;
        if (talking) {
            if (frame->voiced) {
                voiceTones(*frame, tones);
                tones[0].amp *= frame->level;
                tones[1].amp *= frame->level;
                lockLive();
                state.f0 = frame->f0;
                state.f1 = frame->formants[0];
                state.f2 = frame->formants[1];
                unlockLive();
            } else {
                step = 0.004;
                for (size_t i = 0; i < toneCount; ++i)
                    tones[i] = (Tone){randomBetween(2700, 4300), frame->level * 0.4};
            }
        } else {
            int lastHit = i16 % 16;
            while (bassRhythm[lastHit] == '.')
                --lastHit;
            const double age = t - ((i16 / 16) * 16 + lastHit) * sixteenth;
            const double envelope = fmin(1, age / 0.01) * (0.25 + 0.75 * exp(-age * 7));
            tones[0] = (Tone){fmax(1, round(880 / bass)) * bass, 0.45 * envelope * 0.6};
            tones[1] = (Tone){fmax(1, round(1320 / bass)) * bass, 0.45 * envelope * 0.4};
        }
        for (size_t i = firstRinging; i < nextHit; ++i) {
            const struct Hit hit = hits[i];
            const double age = t - hit.time;
            const double duck = talking ? drumDuck[hit.kind] : 1;
            if (hit.kind == 0 && age < 0.14) {
                tones[toneCount++] = (Tone){
                    150 + 850 * exp(-age / 0.018), duck * 0.9 * hit.accent * 1.3 * exp(-age / 0.05),
                };
            } else if (hit.kind == 1 && age < 0.16) {
                const double envelope = duck * 0.9 * hit.accent * exp(-age / 0.045);
                tones[toneCount++] = (Tone){randomBetween(1300, 3800), envelope * 0.55};
                tones[toneCount++] = (Tone){randomBetween(1300, 3800), envelope * 0.45};
            } else if (hit.kind == 2 && age < 0.05 && duck > 0) {
                tones[toneCount++] = (Tone){3951, 0.9 * hit.accent * 0.55 * exp(-age / 0.012)};
            }
        }
        if (nextHit < hitCount) {
            const double untilHit = hits[nextHit].time - t;
            if (untilHit > 0.0035 && untilHit < step)
                step = untilHit;
        }
        for (size_t i = 1; i < toneCount; ++i) {
            Tone tone = tones[i];
            size_t j = i;
            while (j > 0 && tone.amp > tones[j - 1].amp) {
                tones[j] = tones[j - 1];
                --j;
            }
            tones[j] = tone;
        }
        const size_t mixCount = toneCount < 6 ? toneCount : 6;
        double total = 0;
        for (size_t i = 0; i < mixCount; ++i)
            total += tones[i].amp;
        if (total > 0.01)
            play(tones, mixCount, step * 1000, (float)fmin(1, total));
        t += step;
        waitUntil(start + (uint64_t)(t * ticksPerSecond));
    }
    lockLive();
    state.step = -1;
    state.f0 = 0;
    state.f1 = 0;
    state.f2 = 0;
    unlockLive();
}

/* Slide for pitch, move up for vibrato, and press with more of your finger for volume. */
static void touchToPlay(void)
{
    const bool touchStarted = startTouch();
    if (touchStarted) {
        const double waitEnd = nowSeconds() + 8;
        lockLive();
        state.touchWaitUntil = waitEnd;
        unlockLive();
        while (nowSeconds() < waitEnd && !interrupted() && !getContact().down)
            usleep(5000);
    }
    if (!touchStarted || !getContact().down) {
        lockLive();
        state.touchMissed = true;
        unlockLive();
        pauseSeconds(1.5);
        if (touchStarted)
            stopTouch();
        return;
    }
    const double end = nowSeconds() + 12;
    lockLive();
    state.touchSeen = true;
    state.touchEnds = end;
    unlockLive();

    static const char *const notes[] = {"G5", "A5", "B5", "C6", "E6", "F6", "G6", "A6", "B6", "C7"};
    double scale[sizeof(notes) / sizeof(*notes)];
    const int scaleCount = (int)(sizeof(scale) / sizeof(*scale));
    for (int i = 0; i < scaleCount; ++i)
        scale[i] = hz(notes[i]);
    double current = scale[4];
    double downAt = 0;
    bool lastDown = false;
    while (nowSeconds() < end && !interrupted()) {
        Contact contact = getContact();
        lockLive();
        state.touchDown = contact.down;
        if (contact.down) {
            state.touchX = contact.x;
            state.touchY = contact.y;
        }
        unlockLive();
        if (!contact.down) {
            lastDown = false;
            usleep(3000);
            continue;
        }
        if (!lastDown)
            downAt = nowSeconds();
        lastDown = true;

        const int index = (int)fmin(scaleCount - 1, fmax(0, floor((double)contact.x * scaleCount)));
        current *= pow(scale[index] / current, 0.3);
        const double age = nowSeconds() - downAt;
        const double depth = 0.05 + 0.45 * fmax(0, fmin(1, contact.y));
        const double f = current * pow(2, depth * sin(2 * M_PI * 6 * age) / 12);
        const double level = fmin(1, fmax(0.5, 0.3 + contact.size * 0.8));
        const double segment = fmax(1, round(0.02 * f)) / f;
        const uint64_t start = mach_absolute_time();
        play(&(Tone){f, 1}, 1, segment * 1000, (float)level);
        lockLive();
        noteName(f, state.noteName);
        unlockLive();
        waitUntil(start + (uint64_t)(segment * ticksPerSecond));
    }
    lockLive();
    state.touchDown = false;
    unlockLive();
    stopTouch();
}

/* An arpeggio leads up to the most resonant closing note. */
static void sparkle(void)
{
    static const char *const notes[] = {"A5", "C6", "E6", "A6", "C7"};
    for (size_t i = 0; i < sizeof(notes) / sizeof(*notes) && !stopRequested; ++i) {
        const double f = hz(notes[i]);
        const double segment = round(0.07 * f) / f;
        play(&(Tone){f, 1}, 1, segment * 1000, 0.9f);
        waitUntil(mach_absolute_time() + (uint64_t)(segment * ticksPerSecond));
    }
    const uint64_t start = mach_absolute_time();
    const double segment = round(0.02 * 3951) / 3951;
    for (double t = 0; t < 0.6 && !stopRequested;) {
        play(&(Tone){3951, 1}, 1, segment * 1000, (float)exp(-t / 0.18));
        t += segment;
        waitUntil(start + (uint64_t)(t * ticksPerSecond));
    }
}

static const char *saySuggestion(double time)
{
    static const char *const suggestions[] = {
        "can you hear me?", "please stop clicking me", "this is not a speaker",
        "nobody believes me", "i live under the glass", "hello from the trackpad",
    };
    return suggestions[(size_t)(time / 3) % (sizeof(suggestions) / sizeof(*suggestions))];
}

static void sayAnything(void)
{
    for (int i = 0; i < 5 && !stopRequested && !jumpToCredits; ++i) {
        lockLive();
        state.inputActive = true;
        state.inputText[0] = '\0';
        state.inputSubmitted = false;
        state.sayStatus = "typing";
        state.caption[0] = '\0';
        unlockLive();

        double lastChange = nowSeconds();
        char lastText[sizeof(state.inputText)] = "";
        while (!stopRequested && !jumpToCredits) {
            LiveState current = snapshot();
            if (current.inputSubmitted || current.inputDone)
                break;
            if (strcmp(current.inputText, lastText) != 0) {
                memcpy(lastText, current.inputText, sizeof(lastText));
                lastChange = nowSeconds();
            }
            if (nowSeconds() - lastChange > 25) {
                lockLive();
                state.inputDone = true;
                unlockLive();
                break;
            }
            usleep(20000);
        }

        LiveState current = snapshot();
        lockLive();
        state.inputActive = false;
        unlockLive();
        char *text = current.inputText + strspn(current.inputText, " \t");
        size_t length = strlen(text);
        while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'))
            text[--length] = '\0';
        if (current.inputDone || length == 0 || stopRequested || jumpToCredits)
            break;

        lockLive();
        state.sayStatus = "preparing";
        unlockLive();
        Frames frames = render(text);
        lockLive();
        state.sayStatus = "speaking";
        ++state.sayCount;
        unlockLive();
        talk(frames, text);
        free(frames.items);
        pauseSeconds(0.6);
    }
    lockLive();
    state.inputActive = false;
    unlockLive();
}

/* Terminal canvas. The render thread owns both the current and the previous frame. */

enum { canvasWidth = 80, canvasHeight = 24, canvasArea = canvasWidth * canvasHeight };
enum { inkC = 255, bodyC = 251, midC = 247, dimC = 243, ruleC = 240, faintC = 236, paperC = 16 };

typedef struct {
    char glyph[32];
    uint8_t fg, bg;
    bool bold;
} Cell;

typedef struct {
    Cell cells[canvasArea];
    Cell shown[canvasArea];
    bool hasShown;
} Canvas;

static char rgbTable[256][12];
static const char *const spinner[] = {"|", "/", "-", "\\"};

static void applyTheme(bool light)
{
    lightTheme = light;
    const int paper[3] = {light ? 246 : 0, light ? 245 : 0, light ? 241 : 0};
    const int ink = light ? 12 : 255;
    for (int n = 232; n <= 255; ++n) {
        double mix = 0.05 + 0.95 * (n - 232) / 23;
        snprintf(rgbTable[n], sizeof(rgbTable[n]), "%d;%d;%d",
                 (int)(paper[0] + (ink - paper[0]) * mix),
                 (int)(paper[1] + (ink - paper[1]) * mix),
                 (int)(paper[2] + (ink - paper[2]) * mix));
    }
    snprintf(rgbTable[paperC], sizeof(rgbTable[paperC]), "%d;%d;%d", paper[0], paper[1], paper[2]);
    snprintf(screenBackground, sizeof(screenBackground), "\033[48;2;%sm", rgbTable[paperC]);
}

static void formatLocalizedInteger(int number, char *buffer, size_t size)
{
    CFLocaleRef locale = CFLocaleCopyCurrent();
    CFNumberFormatterRef formatter = CFNumberFormatterCreate(NULL, locale, kCFNumberFormatterDecimalStyle);
    CFStringRef formatted = formatter
        ? CFNumberFormatterCreateStringWithValue(NULL, formatter, kCFNumberIntType, &number)
        : NULL;
    if (!formatted || !CFStringGetCString(formatted, buffer, (CFIndex)size, kCFStringEncodingUTF8))
        snprintf(buffer, size, "%d", number);
    if (formatted)
        CFRelease(formatted);
    if (formatter)
        CFRelease(formatter);
    CFRelease(locale);
}

static const char *firstNonASCII(const char *text)
{
    while (*text && (unsigned char)*text < 0x80)
        ++text;
    return text;
}

static int textLength(const char *text)
{
    const char *end = firstNonASCII(text);
    if (!*end)
        return (int)(end - text);
    CFStringRef string = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!string)
        return 0;
    int length = 0;
    for (CFIndex i = 0, end = CFStringGetLength(string); i < end; ++length) {
        CFRange range = CFStringGetRangeOfComposedCharactersAtIndex(string, i);
        i = range.location + range.length;
    }
    CFRelease(string);
    return length;
}

static void putGlyph(Canvas *canvas, int x, int y, const char *glyph, size_t length, uint8_t fg, uint8_t bg, bool bold)
{
    if (x < 0 || x >= canvasWidth || y < 0 || y >= canvasHeight)
        return;
    Cell *cell = &canvas->cells[y * canvasWidth + x];
    if (length >= sizeof(cell->glyph)) {
        glyph = "?";
        length = 1;
    }
    memcpy(cell->glyph, glyph, length);
    cell->glyph[length] = '\0';
    cell->fg = fg;
    cell->bg = bg;
    cell->bold = bold;
}

static void put(Canvas *canvas, int x, int y, const char *glyph, uint8_t fg, bool bold)
{
    putGlyph(canvas, x, y, glyph, strlen(glyph), fg, paperC, bold);
}

static void canvasText(Canvas *canvas, int x, int y, const char *text, uint8_t fg, uint8_t bg, bool bold)
{
    if (!*firstNonASCII(text)) {
        while (*text && x < canvasWidth)
            putGlyph(canvas, x++, y, text++, 1, fg, bg, bold);
        return;
    }

    CFStringRef string = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!string)
        return;
    for (CFIndex i = 0, end = CFStringGetLength(string); i < end && x < canvasWidth; ++x) {
        CFRange range = CFStringGetRangeOfComposedCharactersAtIndex(string, i);
        char glyph[32];
        CFIndex bytes = 0;
        CFIndex copied = CFStringGetBytes(string, range, kCFStringEncodingUTF8, 0, false,
                                         (UInt8 *)glyph, sizeof(glyph) - 1, &bytes);
        if (copied == range.length)
            putGlyph(canvas, x, y, glyph, (size_t)bytes, fg, bg, bold);
        else
            putGlyph(canvas, x, y, "?", 1, fg, bg, bold);
        i = range.location + range.length;
    }
    CFRelease(string);
}

static void drawText(Canvas *canvas, int x, int y, const char *text, uint8_t fg, bool bold)
{
    canvasText(canvas, x, y, text, fg, paperC, bold);
}

static void centerText(Canvas *canvas, int y, const char *text, uint8_t fg, bool bold)
{
    drawText(canvas, (canvasWidth - textLength(text)) / 2, y, text, fg, bold);
}

static void sendChanges(Canvas *canvas, int originX, int originY)
{
    /* Each changed cell may need a cursor position, three attributes and a glyph. */
    char output[canvasArea * 96 + 8];
    char *next = output;
    int writeAt = -1;
    const Cell *pen = NULL;
    for (int i = 0; i < canvasArea; ++i) {
        const Cell *cell = &canvas->cells[i];
        const Cell *old = &canvas->shown[i];
        if (canvas->hasShown && old->fg == cell->fg && old->bg == cell->bg &&
            old->bold == cell->bold && strcmp(old->glyph, cell->glyph) == 0)
            continue;
        if (writeAt != i)
            next += snprintf(next, (size_t)(output + sizeof(output) - next), "\033[%d;%dH",
                             originY + i / canvasWidth + 1, originX + i % canvasWidth + 1);
        if (!pen || pen->bold != cell->bold)
            next += snprintf(next, (size_t)(output + sizeof(output) - next), "%s",
                             cell->bold ? "\033[1m" : "\033[22m");
        if (!pen || pen->fg != cell->fg)
            next += snprintf(next, (size_t)(output + sizeof(output) - next), "\033[38;2;%sm", rgbTable[cell->fg]);
        if (!pen || pen->bg != cell->bg)
            next += snprintf(next, (size_t)(output + sizeof(output) - next), "\033[48;2;%sm", rgbTable[cell->bg]);
        size_t bytes = strlen(cell->glyph);
        memcpy(next, cell->glyph, bytes);
        next += bytes;
        pen = cell;
        writeAt = i % canvasWidth == canvasWidth - 1 ? -1 : i + 1;
    }
    memcpy(canvas->shown, canvas->cells, sizeof(canvas->shown));
    canvas->hasShown = true;
    if (next != output) {
        strcpy(next, "\033[0m");
        out(output);
    }
}

static void dots(const char *label, const char *value, int width, char *buffer, size_t size)
{
    int count = width - textLength(label) - textLength(value) - 2;
    int bytes = snprintf(buffer, size, "%s ", label);
    if (bytes < 0 || (size_t)bytes + 2 >= size)
        return;
    count = count < 1 ? 1 : count;
    if ((size_t)(bytes + count) + 2 > size)
        count = (int)size - bytes - 2;
    memset(buffer + bytes, '.', (size_t)count);
    buffer[bytes + count] = ' ';
    buffer[bytes + count + 1] = '\0';
}

static void asciiBar(double value, int width, char *buffer)
{
    int inner = width - 2;
    int fill = (int)(fmax(0, fmin(1, value)) * inner);
    buffer[0] = '[';
    memset(buffer + 1, ' ', (size_t)inner);
    if (fill) {
        memset(buffer + 1, '=', (size_t)fill - 1);
        buffer[fill] = '>';
    }
    buffer[width - 1] = ']';
    buffer[width] = '\0';
}

static const char *const asciiLogo[] = {
    "    _   __ ____      _____  ____   ______ ___     __ __  ______ ____",
    "   / | / // __ \\    / ___/ / __ \\ / ____//   |   / //_/ / ____// __ \\",
    "  /  |/ // / / /    \\__ \\ / /_/ // __/  / /| |  / ,<   / __/  / /_/ /",
    " / /|  // /_/ /    ___/ // ____// /___ / ___ | / /| | / /___ / _, _/",
    "/_/ |_/ \\____/    /____//_/    /_____//_/  |_|/_/ |_|/_____//_/ |_|",
};

static const struct { char key; const char *rows[5]; } slantGlyphs[] = {
    {'0', {"   ____ ", "  / __ \\", " / / / /", "/ /_/ / ", "\\____/  "}},
    {'1', {"   ___", "  <  /", "  / / ", " / /  ", "/_/   "}},
    {'2', {"   ___ ", "  |__ \\", "  __/ /", " / __/ ", "/____/ "}},
    {'3', {"   _____", "  |__  /", "   /_ < ", " ___/ / ", "/____/  "}},
    {'4', {"   __ __", "  / // /", " / // /_", "/__  __/", "  /_/   "}},
    {'5', {"    ______", "   / ____/", "  /___ \\  ", " ____/ /  ", "/_____/   "}},
    {'6', {"   _____", "  / ___/", " / __ \\ ", "/ /_/ / ", "\\____/  "}},
    {'7', {" _____", "/__  /", "  / / ", " / /  ", "/_/   "}},
    {'8', {"   ____ ", "  ( __ )", " / __  |", "/ /_/ / ", "\\____/  "}},
    {'9', {"   ____ ", "  / __ \\", " / /_/ /", " \\__, / ", "/____/  "}},
    {'H', {"    __  __", "   / / / /", "  / /_/ / ", " / __  /  ", "/_/ /_/   "}},
    {'z', {"      ", " ____ ", "/_  / ", " / /_ ", "/___/ "}},
    {' ', {"   ", "   ", "   ", "   ", "   "}},
};

static const char logoTag[] = "trackpad";

static int logoWidth(void)
{
    int width = 0;
    for (size_t i = 0; i < sizeof(asciiLogo) / sizeof(asciiLogo[0]); ++i) {
        int length = (int)strlen(asciiLogo[i]);
        if (length > width)
            width = length;
    }
    return width;
}

static void printLogoLockup(void)
{
    for (size_t i = 0; i < sizeof(asciiLogo) / sizeof(asciiLogo[0]); ++i)
        printf("  \033[1m%s\033[0m\n", asciiLogo[i]);
    printf("  \033[2m\033[0m\n  \033[2m%*s%s\033[0m\n",
           logoWidth() - 3 - (int)strlen(logoTag), "", logoTag);
}

static void drawLogo(Canvas *canvas, int y)
{
    int width = logoWidth();
    int x0 = (canvasWidth - width) / 2;
    size_t count = sizeof(asciiLogo) / sizeof(asciiLogo[0]);
    for (size_t i = 0; i < count; ++i)
        drawText(canvas, x0, y + (int)i, asciiLogo[i], inkC, true);
    int row = y + (int)count + 1;
    for (int x = x0; x < x0 + width; ++x)
        put(canvas, x, row, "─", dimC, false);
    drawText(canvas, x0 + 1, row, "  haptic voice synthesis  ", midC, false);
    drawText(canvas, x0 + width - 5 - (int)strlen(logoTag), row, "  trackpad  ", inkC, true);
}

static void drawSlant(Canvas *canvas, const char *text, int y)
{
    char rows[5][canvasWidth + 1] = {{0}};
    size_t glyphCount = sizeof(slantGlyphs) / sizeof(slantGlyphs[0]);
    for (const char *c = text; *c; ++c) {
        size_t glyph = 0;
        while (glyph + 1 < glyphCount && slantGlyphs[glyph].key != *c)
            ++glyph;
        for (int row = 0; row < 5; ++row)
            strlcat(rows[row], slantGlyphs[glyph].rows[row], sizeof(rows[row]));
    }
    int width = 0;
    for (int row = 0; row < 5; ++row) {
        int length = (int)strlen(rows[row]);
        if (length > width)
            width = length;
    }
    for (int row = 0; row < 5; ++row) {
        for (int x = 0; rows[row][x]; ++x) {
            if (rows[row][x] != ' ')
                putGlyph(canvas, (canvasWidth - width) / 2 + x, y + row, &rows[row][x], 1, inkC, paperC, true);
        }
    }
}

static uint8_t shimmer(int i)
{
    static const uint8_t silver[] = {245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
                                     255, 254, 253, 252, 251, 250, 249, 248, 247, 246};
    return silver[i % (int)sizeof(silver)];
}

static void copyTextPrefix(const char *text, int count, char *output, size_t size)
{
    if (!*firstNonASCII(text)) {
        size_t length = strlen(text);
        if ((size_t)count < length)
            length = (size_t)count;
        if (length >= size)
            length = size - 1;
        memcpy(output, text, length);
        output[length] = '\0';
        return;
    }
    output[0] = '\0';
    CFStringRef string = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!string)
        return;
    CFIndex end = 0;
    for (CFIndex length = CFStringGetLength(string); end < length && count > 0; --count) {
        CFRange range = CFStringGetRangeOfComposedCharactersAtIndex(string, end);
        end = range.location + range.length;
    }
    CFIndex bytes = 0;
    CFStringGetBytes(string, CFRangeMake(0, end), kCFStringEncodingUTF8, 0, false,
                     (UInt8 *)output, (CFIndex)size - 1, &bytes);
    output[bytes] = '\0';
    CFRelease(string);
}

static void drawCaption(Canvas *canvas, const LiveState *s, double t, int y)
{
    double progress = fmax(0, fmin(1, (t - s->captionStart) / fmax(0.2, s->captionDur) * 1.15));
    char shown[sizeof(s->caption)];
    copyTextPrefix(s->caption, (int)(textLength(s->caption) * progress), shown, sizeof(shown));
    const char *caret = progress >= 1 ? "”" : (int)(t * 3) % 2 == 0 ? "█" : " ";
    char line[sizeof(shown) + 4] = "";
    char previous[sizeof(line)] = "";
    int lines = 0;
    char *remaining = shown;
    char *word;
    while ((word = strsep(&remaining, " ")) != NULL) {
        if (textLength(line) + textLength(word) + 1 > 64) {
            strlcpy(previous, line, sizeof(previous));
            strlcpy(line, word, sizeof(line));
            ++lines;
        } else {
            if (*line)
                strlcat(line, " ", sizeof(line));
            strlcat(line, word, sizeof(line));
        }
    }
    strlcat(line, caret, sizeof(line));
    if (lines <= 1)
        drawText(canvas, 7, y, "“", midC, true);
    if (lines > 0)
        drawText(canvas, 8, y++, previous, inkC, true);
    drawText(canvas, 8, y, line, inkC, true);
}

static void drawProof(Canvas *canvas, const LiveState *s, double t, int y)
{
    if (s->muted < 0)
        return;
    const char *text = s->muted
        ? "   SPEAKERS MUTED  ·  EVERY SOUND YOU HEAR IS THE TRACKPAD   "
        : "   SPEAKERS ARE ON  ·  MUTE YOUR MAC FIRST   ";
    bool blink = !s->muted && (int)(t * 2.5) % 2 == 0;
    canvasText(canvas, (canvasWidth - textLength(text)) / 2, y, text,
               blink ? inkC : paperC, blink ? faintC : inkC, true);
}

static void drawSplashLine(Canvas *canvas, int x, int y, const char *label, const char *value)
{
    char line[96];
    dots(label, value, 56, line, sizeof(line));
    drawText(canvas, x, y, line, midC, false);
    drawText(canvas, x + textLength(line), y, value, inkC, true);
}

static void drawSplash(Canvas *canvas, const LiveState *s, double t)
{
    const int x = 10;
    const int done = s->stepDone;
    char line[128];
    drawText(canvas, x, 11, "$", midC, false);
    copyTextPrefix("./nospeaker", (int)fmax(0, (t - s->started) * 30), line, sizeof(line));
    drawText(canvas, x + 2, 11, line, inkC, true);
    if (done >= 1)
        drawSplashLine(canvas, x + 2, 12, "loading the trackpad's haptics", multitouch ? "ok" : "missing");
    if (done >= 2) {
        drawSplashLine(canvas, x + 2, 13, motorBuiltIn ? "connecting to the built-in trackpad" : "connecting to the trackpad", "ready");
        int total = s->stepTotal > 2 ? s->stepTotal - 2 : 1;
        int voices = (int)fmin(total, done - 2);
        if (voices < total) {
            char bar[31];
            asciiBar((double)voices / total, 30, bar);
            snprintf(line, sizeof(line), "preparing the voice %s %d/%d %s", bar, voices, total, spinner[(int)(t * 10) % 4]);
            drawText(canvas, x + 2, 14, line, bodyC, false);
        } else {
            drawSplashLine(canvas, x + 2, 14, "preparing the voice", "ready");
        }
    }
    drawProof(canvas, s, t, 17);
    centerText(canvas, 19, "keep your hands off the trackpad until the end", dimC, false);
    bool ready = s->stepTotal > 0 && done >= s->stepTotal;
    centerText(canvas, 22, ready && s->muted != 1 ? "space to start anyway  ·  q end" : "space skip  ·  q end", dimC, false);
}

static void drawHeart(Canvas *canvas, const LiveState *s, double t)
{
    static const char *const heartArt[] = {",d88b.d88b,", "88888888888", "`Y8888888Y'", "  `Y888Y'", "    `Y'"};
    static const uint8_t shades[] = {236, 238, 240, 243, 246, 249, 252, 255};
    double pulse = fmax(0, 1 - (t - s->lastPlay) / 0.35) * s->level;
    int color = shades[(int)fmin(7, pulse * 7 + 0.5)];
    for (int y = 0; y < 5; ++y)
        drawText(canvas, 34, 11 + y, heartArt[y], (uint8_t)color, pulse > 0.3);
    centerText(canvas, 19, "waking up the motor", pulse > 0.3 ? inkC : dimC, pulse > 0.3);
}

static int xForHz(double f)
{
    return 3 + (int)(log(fmax(100, fmin(4000, f)) / 100) / log(40) * 73);
}

static void drawSweep(Canvas *canvas, const LiveState *s, double t)
{
    double f = t - s->lastPlay < 0.1 ? s->hz : 0;
    char label[32];
    snprintf(label, sizeof(label), "%d Hz", f > 0 ? (int)f : 100);
    drawSlant(canvas, label, 10);
    static const char levels[] = "_.-~^";
    double cycles = 1 + 22 * log(fmax(100, f) / 100) / log(40);
    for (int x = 0; x < 76; ++x) {
        double v = f > 0 ? sin(2 * M_PI * cycles * x / 76 - t * 12) : 0;
        char glyph = f > 0 ? levels[(int)fmin(4, (v + 1) / 2 * 4.99)] : '-';
        putGlyph(canvas, 2 + x, 16, &glyph, 1, f > 0 ? shimmer(x / 3 + (int)(t * 10)) : faintC, paperC, true);
    }
    for (int x = 3; x <= 76; ++x)
        put(canvas, x, 18, "-", faintC, false);
    static const struct { double frequency; const char *label; } marks[] = {
        {100, "100"}, {250, "250"}, {500, "500"}, {1000, "1k"}, {2000, "2k"}, {4000, "4k"}
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
        int x = xForHz(marks[i].frequency);
        put(canvas, x, 18, "+", dimC, false);
        drawText(canvas, (int)fmax(2, x - (int)strlen(marks[i].label) / 2), 19, marks[i].label, midC, false);
    }
    if (f > 0)
        put(canvas, xForHz(f), 17, "v", inkC, true);
}

static void meter(Canvas *canvas, int y, const char *label, double value, double lo, double hi)
{
    drawText(canvas, 6, y, label, inkC, true);
    char bar[47], frequency[32];
    asciiBar(value > 0 ? (value - lo) / (hi - lo) : 0, 46, bar);
    drawText(canvas, 10, y, bar, bodyC, false);
    snprintf(frequency, sizeof(frequency), "%5.0f hz", value);
    drawText(canvas, 57, y, value > 0 ? frequency : "    -   ", value > 0 ? inkC : dimC, false);
}

static void drawTalk(Canvas *canvas, const LiveState *s, double t)
{
    drawText(canvas, 6, 10, "▸ SPEAKING", inkC, true);
    drawCaption(canvas, s, t, 11);
    bool on = t - s->lastPlay < 0.08;
    meter(canvas, 14, "F0", on ? s->f0 : 0, 60, 260);
    meter(canvas, 15, "F1", on ? s->f1 : 0, 150, 1000);
    meter(canvas, 16, "F2", on ? s->f2 : 0, 600, 2800);
    static const char ramp[] = " .:-=+*#%@";
    for (int x = 3; x <= 76; ++x) {
        double fx = 100 * pow(40, (x - 3) / 73.0);
        double value = 0;
        if (on) {
            if (s->f1 > 0)
                value += exp(-pow(log(fx / s->f1) / 0.12, 2));
            if (s->f2 > 0)
                value += 0.8 * exp(-pow(log(fx / s->f2) / 0.12, 2));
            value *= s->level;
        }
        int shade = (int)(fmax(0, fmin(1, value)) * 9 + 0.5);
        char glyph = shade ? ramp[shade] : '.';
        putGlyph(canvas, x, 18, &glyph, 1, shade ? inkC : faintC, paperC, false);
        put(canvas, x, 19, "-", faintC, false);
    }
    drawText(canvas, 3, 20, "100", dimC, false);
    drawText(canvas, xForHz(1000) - 1, 20, "1k", dimC, false);
    drawText(canvas, 74, 20, "4k", dimC, false);
    drawText(canvas, 25, 20, "two tones, one motor, no speaker", midC, false);
}

enum { rollLength = 62, trailLength = 14 };
static int rollHistory[rollLength];
static struct { double x, y; } trail[trailLength];
static int trailCount;
static int marquee;

static void remember(const LiveState *s, double t)
{
    memmove(rollHistory, rollHistory + 1, (rollLength - 1) * sizeof(rollHistory[0]));
    rollHistory[rollLength - 1] = s->scene == SCENE_SING && t - s->lastPlay < 0.1 ? s->noteRow : -1;
    if (s->scene == SCENE_TOUCH && s->touchDown) {
        if (trailCount == trailLength) {
            memmove(trail, trail + 1, (trailLength - 1) * sizeof(trail[0]));
            --trailCount;
        }
        trail[trailCount].x = s->touchX;
        trail[trailCount++].y = s->touchY;
    }
}

static void drawSing(Canvas *canvas, const LiveState *s)
{
    for (int i = 0; i < (int)(sizeof(rollNotes) / sizeof(rollNotes[0])); ++i) {
        int y = 10 + i;
        drawText(canvas, 2, y, rollNotes[i], s->noteRow == i ? inkC : dimC, s->noteRow == i);
        for (int x = 6; x < 68; ++x)
            put(canvas, x, y, "·", faintC, false);
    }
    for (int i = 0; i < rollLength; ++i) {
        if (rollHistory[i] >= 0) {
            bool isNow = i == rollLength - 1;
            put(canvas, 6 + i, 10 + rollHistory[i], isNow ? "o" : "=", isNow ? inkC : shimmer(i / 3), true);
        }
    }
    drawText(canvas, 70, 11, "»", inkC, true);
    if (s->noteRow >= 0) {
        char frequency[32];
        drawText(canvas, 72, 11, s->noteName, inkC, true);
        snprintf(frequency, sizeof(frequency), "%4.0fhz", s->hz);
        drawText(canvas, 70, 12, frequency, midC, false);
    }
    centerText(canvas, 17, "an original hook in the motor's loudest, cleanest octave", bodyC, false);
    centerText(canvas, 18, "scoops · glides · vibrato", dimC, false);
}

static void drawBeat(Canvas *canvas, const LiveState *s, double t)
{
    const char *const labels[] = {"KICK", "SNARE", "HAT"};
    double hits[] = {s->kick, s->snare, s->hat};
    const int top = 10, height = 3, width = 22;
    for (int i = 0; i < 3; ++i) {
        int x0 = 4 + i * 25;
        bool hot = t - hits[i] < 0.13;
        for (int y = top; y < top + height; ++y) {
            for (int x = x0; x < x0 + width; ++x) {
                bool edgeY = y == top || y == top + height - 1;
                bool edgeX = x == x0 || x == x0 + width - 1;
                char glyph = edgeY ? (edgeX ? '+' : '-') : (edgeX ? '|' : ' ');
                putGlyph(canvas, x, y, &glyph, 1, hot ? paperC : ruleC, hot ? inkC : paperC, false);
            }
        }
        canvasText(canvas, x0 + (width - (int)strlen(labels[i])) / 2, top + 1,
                   labels[i], hot ? paperC : bodyC, hot ? inkC : paperC, true);
    }

    static const char *const patterns[] = {"X.........X..x..", "....X.......X...", "x.x.x.x.x.x.xxx."};
    static const char *const laneLabels[] = {"K", "S", "H"};
    for (int lane = 0; lane < 3; ++lane) {
        drawText(canvas, 8, 14 + lane, laneLabels[lane], inkC, true);
        for (int i = 0; patterns[lane][i]; ++i) {
            bool isStep = i == s->step;
            bool filled = patterns[lane][i] != '.';
            canvasText(canvas, 10 + i * 4, 14 + lane, filled ? " x " : " . ",
                       isStep ? paperC : filled ? inkC : dimC,
                       isStep ? (filled ? inkC : midC) : paperC, filled);
        }
    }
    if (s->scene == SCENE_RAP) {
        drawCaption(canvas, s, t, 18);
    } else {
        char line[128];
        snprintf(line, sizeof(line), "%.0f bpm  ·  bar %d/%d  ·  kick, snare, hats and bass from one motor",
                 s->bpm, s->bar + 1, s->bars);
        centerText(canvas, 19, line, midC, false);
    }
}

static void drawYourTurn(Canvas *canvas, const LiveState *s, double t)
{
    centerText(canvas, 11, "Y O U R   T U R N", inkC, true);
    drawCaption(canvas, s, t, 14);
    centerText(canvas, 18, "in a moment you'll play it with your finger", midC, false);
}

static void drawTouch(Canvas *canvas, const LiveState *s, double t)
{
    const int x0 = 3, y0 = 10, w = 38, h = 11;
    const uint8_t edge = s->touchDown ? inkC : dimC;
    put(canvas, x0, y0, "╭", edge, false);
    put(canvas, x0 + w - 1, y0, "╮", edge, false);
    put(canvas, x0, y0 + h - 1, "╰", edge, false);
    put(canvas, x0 + w - 1, y0 + h - 1, "╯", edge, false);
    for (int x = x0 + 1; x < x0 + w - 1; ++x) {
        put(canvas, x, y0, "─", edge, false);
        put(canvas, x, y0 + h - 1, "─", edge, false);
    }
    for (int y = y0 + 1; y < y0 + h - 1; ++y) {
        put(canvas, x0, y, "│", edge, false);
        put(canvas, x0 + w - 1, y, "│", edge, false);
    }
    static const char *const names[] = {"G5", "C6", "G6", "C7"};
    for (int i = 0; i < 4; ++i)
        drawText(canvas, x0 + 2 + i * 3 * 34 / 10, y0 + h - 2, names[i], ruleC, false);
    for (int i = 0; i < trailCount; ++i) {
        int x = x0 + 1 + (int)(trail[i].x * (w - 3));
        int y = y0 + 1 + (int)((1 - trail[i].y) * (h - 3));
        bool isNow = i == trailCount - 1 && s->touchDown;
        put(canvas, x, y, isNow ? "@" : "o", isNow ? inkC : shimmer(i), true);
    }
    const int rx = 45;
    char line[64], bar[23];
    if (s->touchMissed) {
        drawText(canvas, rx, 12, "no touch, skipping", dimC, false);
    } else if (!s->touchSeen) {
        bool blink = (int)(t * 3) % 2 == 0;
        drawText(canvas, rx, 11, "▶ PUT A FINGER ON", blink ? inkC : midC, true);
        drawText(canvas, rx, 12, "  THE TRACKPAD", blink ? inkC : midC, true);
        drawText(canvas, rx, 14, "glide lightly, no need to click", midC, false);
        snprintf(line, sizeof(line), "waiting %.0fs", fmax(0, s->touchWaitUntil - t));
        drawText(canvas, rx, 16, line, dimC, false);
    } else {
        drawText(canvas, rx, 10, "TOUCH TO PLAY", inkC, true);
        drawText(canvas, rx, 12, "slide left/right   pitch", bodyC, false);
        drawText(canvas, rx, 13, "move up            vibrato", bodyC, false);
        drawText(canvas, rx, 14, "glide lightly, don't click", dimC, false);
        bool on = s->touchDown && t - s->lastPlay < 0.08;
        drawText(canvas, rx, 16, "note", dimC, false);
        drawText(canvas, rx + 6, 16, on ? s->noteName : "-", inkC, true);
        if (on) {
            snprintf(line, sizeof(line), "%4.0f hz", s->hz);
            drawText(canvas, rx + 12, 16, line, midC, false);
        }
        asciiBar(on ? s->level : 0, 22, bar);
        drawText(canvas, rx, 17, bar, bodyC, false);
        double left = fmax(0, s->touchEnds - t);
        asciiBar(left / 12, 22, bar);
        drawText(canvas, rx, 19, bar, inkC, false);
        snprintf(line, sizeof(line), "%2.0fs", left);
        drawText(canvas, rx + 22, 19, line, dimC, false);
    }
}

static void drawSay(Canvas *canvas, const LiveState *s, double t)
{
    drawText(canvas, 6, 10, "▸ SAY ANYTHING", inkC, true);
    drawText(canvas, 6, 11, "type something and press return. it'll say it.", midC, false);
    const int w = 68, x0 = 6;
    char line[128];
    uint8_t color = s->inputActive ? bodyC : ruleC;
    line[0] = '+';
    memset(line + 1, '-', (size_t)w - 2);
    line[w - 1] = '+';
    line[w] = '\0';
    drawText(canvas, x0, 12, line, color, false);
    drawText(canvas, x0, 14, line, color, false);
    drawText(canvas, x0, 13, "|", color, false);
    drawText(canvas, x0 + w - 1, 13, "|", color, false);
    if (s->inputActive) {
        /* Input is printable ASCII, as in the original key handler. */
        size_t length = strlen(s->inputText);
        const char *shown = s->inputText + (length > (size_t)w - 6 ? length - ((size_t)w - 6) : 0);
        snprintf(line, sizeof(line), "%s%c", shown, (int)(t * 2.5) % 2 == 0 ? '_' : ' ');
        drawText(canvas, x0 + 2, 13, ">", midC, false);
        drawText(canvas, x0 + 4, 13, line, inkC, true);
        if (!*s->inputText) {
            snprintf(line, sizeof(line), "try: %s", saySuggestion(t));
            drawText(canvas, x0 + 6, 13, line, dimC, false);
            drawText(canvas, x0 + w - 3 - 13, 13, "tab to use it", faintC, false);
        }
    }
    if (strcmp(s->sayStatus, "preparing") == 0) {
        snprintf(line, sizeof(line), "preparing the voice %s", spinner[(int)(t * 10) % 4]);
        drawText(canvas, 6, 16, line, midC, false);
    } else if (strcmp(s->sayStatus, "speaking") == 0) {
        drawCaption(canvas, s, t, 16);
    } else if (s->sayCount > 0) {
        drawText(canvas, 6, 16, "type another one, or finish below", midC, false);
    }
    const char *finish = "when you're done, press  ESC  to see the credits";
    drawText(canvas, 6, 19, finish, inkC, true);
    const char *escape = strstr(finish, " ESC ");
    canvasText(canvas, 6 + (int)(escape - finish), 19, " ESC ", paperC, inkC, true);
    drawText(canvas, 8 + (int)strlen(finish), 19, "· return says it", dimC, false);
}

static void drawOutro(Canvas *canvas, const LiveState *s, double t)
{
    char vibrations[48], runtime[32];
    formatLocalizedInteger(s->actuations, vibrations, sizeof(vibrations));
    snprintf(runtime, sizeof(runtime), "%.1f s", t - s->started);
    const char *const rows[][2] = {
        {"code · sound · design", "a90"}, {"web", "a90.com"}, {"speakers used", "0"},
        {"vibrations", vibrations}, {"runtime", runtime}
    };
    centerText(canvas, 10, "C R E D I T S", inkC, true);
    for (int i = 0; i < 5; ++i) {
        char line[96];
        dots(rows[i][0], rows[i][1], 52, line, sizeof(line));
        drawText(canvas, 14, 12 + i, line, dimC, false);
        drawText(canvas, 14 + textLength(line), 12 + i, rows[i][1], inkC, true);
    }
    if (s->waitingForKey) {
        const char *prefix = "follow @codycollier for more  ·  press ";
        const char *line = "follow @codycollier for more  ·  press  F  to open";
        int x = (canvasWidth - textLength(line)) / 2;
        drawText(canvas, x, 18, line, inkC, true);
        canvasText(canvas, x + textLength(prefix), 18, " F ", paperC, inkC, true);
        if (t - s->followOpenedAt < 2.5) {
            centerText(canvas, 19, "opened x.com/codycollier in your browser", bodyC, false);
        } else {
            bool blink = (int)(t * 2) % 2 == 0;
            centerText(canvas, 19, "any other key exits", blink ? midC : dimC, false);
        }
    } else {
        centerText(canvas, 19, "thanks for listening", dimC, false);
    }
}

static const char scrollText[] =
    "      ·  a90.com presents  NO SPEAKER  ·    the trackpad has no speaker. every sound you hear is its haptic motor, "
    "driven through MultitouchSupport, a private macOS framework.    range: 100 to 4000 vibrations per second.    voice: "
    "two formants, retriggered on every pitch beat.    thanks for listening.    a90.com    ";
static const char *scrollPosition = scrollText;

static const char *nextScrollGlyph(const char *glyph)
{
    /* The only multibyte glyph in the scroller is the middle dot. */
    const char *next = glyph + ((unsigned char)*glyph < 0x80 ? 1 : 2);
    return *next ? next : scrollText;
}

static void draw(Canvas *canvas, const LiveState *s, double t)
{
    const Cell blank = {.glyph = " ", .fg = 250, .bg = paperC};
    for (int i = 0; i < canvasArea; ++i)
        canvas->cells[i] = blank;

    const char presents[] = "a90.com presents";
    int width = logoWidth();
    int left = (canvasWidth - width) / 2;
    int side = (width - ((int)sizeof(presents) - 1) - 4) / 2;
    int rightStart = left + side + (int)sizeof(presents) + 3;
    for (int x = left; x < left + side; ++x)
        put(canvas, x, 0, "─", dimC, false);
    for (int x = rightStart; x < left + width; ++x)
        put(canvas, x, 0, "─", dimC, false);
    drawText(canvas, left + side + 2, 0, presents, bodyC, true);
    drawLogo(canvas, 2);

    switch (s->scene) {
    case SCENE_INTRO:
        drawSplash(canvas, s, t);
        return;
    case SCENE_HEART:
        drawHeart(canvas, s, t);
        break;
    case SCENE_SWEEP:
        drawSweep(canvas, s, t);
        break;
    case SCENE_TALK:
        drawTalk(canvas, s, t);
        break;
    case SCENE_SING:
        drawSing(canvas, s);
        break;
    case SCENE_BEAT:
    case SCENE_RAP:
        drawBeat(canvas, s, t);
        break;
    case SCENE_YOUR_TURN:
        drawYourTurn(canvas, s, t);
        break;
    case SCENE_TOUCH:
        drawTouch(canvas, s, t);
        break;
    case SCENE_SAY:
        drawSay(canvas, s, t);
        break;
    case SCENE_OUTRO:
        drawOutro(canvas, s, t);
        break;
    }

    char badge[32], frequency[32], bar[11];
    snprintf(badge, sizeof(badge), " %02d/%02d ", s->part, partCount);
    canvasText(canvas, 1, 21, badge, paperC, inkC, true);
    drawText(canvas, 2 + (int)strlen(badge), 21, s->partName, inkC, true);
    bool active = t - s->lastPlay < 0.08;
    drawText(canvas, 24, 21, "motor", dimC, false);
    snprintf(frequency, sizeof(frequency), "%4.0f hz", s->hz);
    drawText(canvas, 30, 21, active ? frequency : "idle", active ? inkC : dimC, active);
    asciiBar(active ? s->level : 0, 10, bar);
    drawText(canvas, 38, 21, bar, bodyC, false);
    if (s->muted >= 0) {
        const char *chip = s->muted ? " SPEAKERS MUTED " : " SPEAKERS ON · MUTE ";
        bool blink = !s->muted && (int)(t * 2.5) % 2 == 0;
        canvasText(canvas, 79 - textLength(chip), 21, chip,
                   blink ? inkC : paperC, blink ? faintC : inkC, true);
    }
    drawText(canvas, 2, 22, "space skip · q end", faintC, false);
    const char *glyph = scrollPosition;
    for (int x = 0; x < canvasWidth; ++x) {
        size_t length = (unsigned char)*glyph < 0x80 ? 1 : 2;
        putGlyph(canvas, x, 23, glyph, length, shimmer((x + marquee) / 2), paperC, true);
        glyph = nextScrollGlyph(glyph);
    }
}

static _Atomic bool renderRunning;
static pthread_t renderer;

static void *renderMain(void *unused)
{
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    Canvas canvas = {0};
    int lastW = 0, lastH = 0;
    unsigned int frameCount = 0;
    double lastMutePoll = -9;
    double next = nowSeconds();
    while (renderRunning) {
        double t = nowSeconds();
        ++frameCount;
        LiveState s = snapshot();
        remember(&s, t);
        if (t - lastMutePoll > 0.5) {
            lastMutePoll = t;
            int muted = speakersMuted();
            lockLive();
            state.muted = muted;
            unlockLive();
        }
        int w, h;
        terminalSize(&w, &h);
        if (w != lastW || h != lastH) {
            fillScreen();
            lastW = w;
            lastH = h;
            canvas.hasShown = false;
        }
        draw(&canvas, &s, t);
        int x = w > canvasWidth ? (w - canvasWidth) / 2 : 0;
        int y = h > canvasHeight ? (h - canvasHeight) / 2 : 0;
        sendChanges(&canvas, x, y);
        if (s.waitingForKey || frameCount % 2 == 0) {
            ++marquee;
            scrollPosition = nextScrollGlyph(scrollPosition);
        }
        next += s.waitingForKey ? 0.125 : 1.0 / 24;
        double wait = next - nowSeconds();
        if (wait > 0)
            usleep((useconds_t)(wait * 1e6));
        else
            next = nowSeconds();
    }
    return NULL;
}

static void startRenderer(void)
{
    for (int i = 0; i < rollLength; ++i)
        rollHistory[i] = -1;
    renderRunning = true;
    if (pthread_create(&renderer, NULL, renderMain, NULL) != 0) {
        renderRunning = false;
        fail("It couldn't start the terminal renderer.", NULL, 1);
    }
}

static void stopRenderer(void)
{
    if (!renderRunning)
        return;
    renderRunning = false;
    pthread_join(renderer, NULL);
}
// Keyboard input for the interactive show.
static atomic_bool keysRunning;
static pthread_t keyboardThread;

static bool waitForInput(int milliseconds)
{
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    return poll(&input, 1, milliseconds) > 0 && (input.revents & POLLIN);
}

// Ignore complete ANSI mouse/arrow sequences. Return false for a plain Escape.
static bool skipEscapeSequence(void)
{
    if (!waitForInput(30))
        return false;

    unsigned char byte;
    if (read(STDIN_FILENO, &byte, 1) == 1 && (byte == '[' || byte == 'O')) {
        while (waitForInput(30) && read(STDIN_FILENO, &byte, 1) == 1) {
            if (byte >= 0x40 && byte <= 0x7e)
                break;
        }
    }
    return true;
}

static void *readKeys(void *unused)
{
    (void)unused;
    pid_t profileChild = 0;
    while (keysRunning && !stopRequested) {
        if (profileChild) {
            pid_t result = waitpid(profileChild, NULL, WNOHANG);
            if (result > 0 || (result < 0 && errno != EINTR))
                profileChild = 0;
        }
        unsigned char key;
        if (!waitForInput(100) || read(STDIN_FILENO, &key, 1) != 1) {
            usleep(10000);
            continue;
        }

        LiveState current = snapshot();
        if (current.inputActive) {
            if (key == 27 && skipEscapeSequence())
                continue;
            lockLive();
            size_t length = strlen(state.inputText);
            if (key == 27)
                state.inputDone = true;
            else if (key == '\r' || key == '\n')
                state.inputSubmitted = true;
            else if (key == '\t' && length == 0)
                snprintf(state.inputText, sizeof(state.inputText), "%s", saySuggestion(nowSeconds()));
            else if ((key == 127 || key == 8) && length)
                state.inputText[length - 1] = '\0';
            else if (key >= 32 && key <= 126 && length < 70) {
                state.inputText[length] = (char)key;
                state.inputText[length + 1] = '\0';
            }
            unlockLive();
            continue;
        }
        if (key == 27) {
            if (!skipEscapeSequence())
                requestEnd();
            continue;
        }
        if ((key == 'f' || key == 'F') && current.waitingForKey) {
            char *arguments[] = {"/usr/bin/open", "https://x.com/codycollier", NULL};
            pid_t child;
            if (!profileChild && posix_spawn(&child, arguments[0], NULL, NULL, arguments, environ) == 0)
                profileChild = child;
            lockLive();
            state.followOpenedAt = nowSeconds();
            unlockLive();
            continue;
        }
        keyPressed = true;
        if (key == 'q' || key == 'Q')
            requestEnd();
        if (key == ' ' || key == 's')
            skipRequested = true;
    }
    return NULL;
}

static void startKeys(void)
{
    keysRunning = true;
    if (pthread_create(&keyboardThread, NULL, readKeys, NULL) != 0) {
        keysRunning = false;
        fail("It couldn't start the keyboard reader.", NULL, 1);
    }
}

static void stopKeys(void)
{
    keysRunning = false;
    pthread_join(keyboardThread, NULL);
}

static void greetingLine(char line[128])
{
    time_t now = time(NULL);
    struct tm local;
    int hour = localtime_r(&now, &local) ? local.tm_hour : 12;
    const char *hello = hour < 5 ? "You're up late." :
                        hour < 12 ? "Good morning." :
                        hour < 17 ? "Good afternoon." :
                        hour < 22 ? "Good evening." : "You're up late.";
    snprintf(line, 128, "%s I'm your trackpad. I don't have a speaker. I've been meaning to say something.", hello);
}

static void signOff(const char *program, int seconds, int actuations)
{
    char vibrations[64];
    formatLocalizedInteger(actuations, vibrations, sizeof(vibrations));
    const char *home = getenv("HOME");
    const char *me = strchr(program, '/') ? program : "nospeaker";
    size_t homeLength = home ? strlen(home) : 0;
    bool shorten = homeLength && strncmp(me, home, homeLength) == 0 && me[homeLength] == '/';
    const char *prefix = shorten ? "~" : "";
    if (shorten)
        me += homeLength;

    puts("");
    printLogoLockup();
    puts("");
    printf("  \033[1m%d:%02d\033[0m  ·  \033[1m%s\033[0m vibrations  ·  \033[1m0\033[0m speakers used\n",
           seconds / 60, seconds % 60, vibrations);
    puts("  more        \033[1mx.com/codycollier\033[0m");
    printf("  play again  \033[2m%s%s\033[0m\n", prefix, me);
    printf("  say a line  \033[2m%s%s \"hello there\"\033[0m\n\n", prefix, me);
}

static void show(const char *program, bool visuals)
{
    state.started = nowSeconds();
    if (visuals) {
        if (!takeTerminal())
            fail("It couldn't switch the terminal to interactive input.", NULL, 1);
        startKeys();
        startRenderer();
    }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    enter(SCENE_INTRO, 1, "LOADING");
    if (!visuals)
        puts("a90.com presents NO SPEAKER. Your trackpad doesn't have one. In a moment it's going to talk.");
    double introStart = nowSeconds();
    char greeting[128];
    greetingLine(greeting);
    const char *rapLine = "No speaker. No wires. Just a motor and a beat.";
    const char *turnLine = "Your turn. Be gentle.";
    const char *byeLine = "Okay. Back to clicking.";
    const char *phrases[] = {greeting, rapLine, turnLine, byeLine};
    Frames voices[sizeof(phrases) / sizeof(*phrases)] = {0};
    const int steps = 2 + (int)(sizeof(voices) / sizeof(*voices));

    lockLive();
    state.stepTotal = steps;
    unlockLive();
    pauseSeconds(0.6);
    lockLive();
    state.stepDone = 1;
    unlockLive();
    pauseSeconds(0.5);
    for (int i = 0; i < steps - 2 && !stopRequested; i++) {
        lockLive();
        state.stepDone = 2 + i;
        unlockLive();
        voices[i] = render(phrases[i]);
    }
    lockLive();
    state.stepDone = steps;
    unlockLive();
    pauseSeconds(fmax(0.6, 4.0 - (nowSeconds() - introStart)));
    if (visuals) {
        double holdUntil = nowSeconds() + 15;
        while (speakersMuted() == 0 && !interrupted() && nowSeconds() < holdUntil)
            usleep(100000);
        if (speakersMuted() == 1) {
            lockLive();
            state.muted = 1;
            unlockLive();
            pauseSeconds(1.2);
        }
        skipRequested = false;
    }

    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_HEART, 2, "WAKING UP");
        pauseSeconds(0.3);
        heartbeat();
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_TALK, 3, "FIRST WORDS");
        if (!visuals)
            printf("“%s”\n", greeting);
        pauseSeconds(0.4);
        talk(voices[0], greeting);
        pauseSeconds(0.9);
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_SWEEP, 4, "SWEEP");
        if (!visuals)
            puts("sweep 100 Hz → 4 kHz");
        pauseSeconds(0.3);
        sweep(6);
        pauseSeconds(0.4);
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_SING, 5, "MELODY");
        if (!visuals)
            puts("♪ melody");
        pauseSeconds(0.4);
        sing();
        pauseSeconds(0.5);
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_BEAT, 6, "BEAT");
        if (!visuals)
            printf("beat · then rap: “%s”\n", rapLine);
        groove(4, 2, voices[1], rapLine);
        pauseSeconds(0.5);
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_YOUR_TURN, 7, "YOUR TURN");
        if (!visuals)
            printf("“%s”\n", turnLine);
        pauseSeconds(0.3);
        talk(voices[2], turnLine);
        pauseSeconds(0.4);
    }
    if (!stopRequested && !jumpToCredits) {
        enter(SCENE_TOUCH, 8, "TOUCH TO PLAY");
        if (!visuals)
            puts("touch the trackpad: slide for pitch (12 s)");
        touchToPlay();
    }
    if (!stopRequested && !jumpToCredits && visuals) {
        enter(SCENE_SAY, 9, "SAY ANYTHING");
        sayAnything();
    }
    if (!stopRequested) {
        jumpToCredits = false;
        inCredits = true;
        enter(SCENE_OUTRO, 10, "OUTRO");
        if (!voices[3].items)
            voices[3] = render(byeLine);
        talk(voices[3], byeLine);
        lockLive();
        state.scene = SCENE_OUTRO;
        unlockLive();
        sparkle();
        if (visuals) {
            keyPressed = false;
            lockLive();
            state.waitingForKey = true;
            unlockLive();
            while (!keyPressed && !stopRequested)
                usleep(20000);
        } else {
            pauseSeconds(1);
        }
    }

    for (size_t i = 0; i < sizeof(voices) / sizeof(*voices); i++)
        free(voices[i].items);
    LiveState final = snapshot();
    if (visuals) {
        stopRenderer();
        stopKeys();
        restoreTerminal();
    }
    signOff(program, (int)(nowSeconds() - final.started), final.actuations);
}

// Command line arguments are consumed in the same order as the Swift CLI.
static int findArgument(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0)
            return i;
    }
    return 0;
}

static void removeArguments(int *argc, char **argv, int index, int count)
{
    memmove(argv + index, argv + index + count,
            (size_t)(*argc - index - count + 1) * sizeof(*argv));
    *argc -= count;
}

static bool flag(int *argc, char **argv, const char *name)
{
    int index = findArgument(*argc, argv, name);
    if (!index)
        return false;
    removeArguments(argc, argv, index, 1);
    return true;
}

static char *joinWords(int count, char **words)
{
    size_t length = 1;
    for (int i = 0; i < count; i++)
        length += strlen(words[i]) + (i > 0);
    char *text = malloc(length);
    if (!text)
        fail("It couldn't allocate the text to speak.", NULL, 1);

    char *cursor = text;
    for (int i = 0; i < count; i++) {
        if (i)
            *cursor++ = ' ';
        size_t bytes = strlen(words[i]);
        memcpy(cursor, words[i], bytes);
        cursor += bytes;
    }
    *cursor = '\0';
    return text;
}

int main(int argc, char **argv)
{
    if (findArgument(argc, argv, "--help") || findArgument(argc, argv, "-h")) {
        puts("NO SPEAKER · a90.com presents\n"
             "Your MacBook's trackpad has no speaker. This makes it talk anyway, through its haptic motor.\n\n"
             "  nospeaker                         the show (space skips a part, q goes to the end)\n"
             "  nospeaker \"any words\"             just say something\n"
             "  nospeaker --sweep                 a rising tone, 100 Hz to 4 kHz\n"
             "  --voice Daniel                    any voice from `say -v '?'`\n"
             "  --dark                            white on black instead of black on white\n"
             "  --plain                           no visuals\n\n"
             "Mute your Mac: it still talks. Put it on a table, and keep your hands off the trackpad until it asks.\n"
             "a90.com · x.com/codycollier");
        return 0;
    }

    int voiceIndex = findArgument(argc, argv, "--voice");
    if (voiceIndex) {
        if (voiceIndex + 1 >= argc || argv[voiceIndex + 1][0] == '-')
            usageError("--voice needs a value, like: --voice Daniel");
        voiceName = argv[voiceIndex + 1];
        removeArguments(&argc, argv, voiceIndex, 2);
        if (!voiceExists(voiceName))
            badVoice(voiceName, 64);
    }
    applyTheme(!flag(&argc, argv, "--dark"));
    bool plain = flag(&argc, argv, "--plain");
    bool wantsSweep = flag(&argc, argv, "--sweep");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            char *message;
            if (asprintf(&message, "\"%s\" isn't an option.", argv[i]) < 0)
                usageError("Unknown option.");
            usageError(message);
        }
    }

    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    ticksPerSecond = 1e9 * timebase.denom / timebase.numer;
    startTicks = mach_absolute_time();
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    atexit(restoreTerminal);
    atexit(closeMotor);
    openMotor();

    if (wantsSweep || argc > 1) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        if (wantsSweep) {
            puts("sweeping 100 Hz → 4 kHz…");
            sweep(5);
        }
        if (argc > 1) {
            char *text = joinWords(argc - 1, argv + 1);
            Frames frames = render(text);
            printf("trackpad: “%s”\n", text);
            talk(frames, text);
            free(frames.items);
            free(text);
        }
    } else {
        int width, height;
        terminalSize(&width, &height);
        bool interactive = !plain && isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
        if (interactive && (width < 80 || height < 24)) {
            puts("\n  nospeaker needs a slightly bigger window: at least 80 × 24.");
            puts("  Drag the window's corner to make it bigger, and it starts by itself.  (ctrl-c to stop)\n");
            while ((width < 80 || height < 24) && !stopRequested && !jumpToCredits) {
                usleep(200000);
                terminalSize(&width, &height);
            }
            if (stopRequested || jumpToCredits)
                return 0;
        }
        show(argv[0], interactive && width >= 80 && height >= 24);
    }
    usleep(120000);
    return 0;
}
