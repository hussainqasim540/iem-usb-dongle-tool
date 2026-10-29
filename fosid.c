// fosid — demone leggero per le IEM collegate al dongle Fosi Audio D2.
//
//   * scollegamento del dongle     -> pausa di tutto quello che sta suonando
//   * 1 click                      -> pausa di tutte le app che riproducono
//   * pressione lunga              -> riprende un'app alla volta tra quelle messe in pausa
//   * 2 click / 3 click            -> traccia successiva / precedente
//
// Prende in esclusiva (seize) solo l'interfaccia HID del Fosi, quindi tastiera,
// tasti multimediali e SoundSource continuano a funzionare come prima.
// macOS lascia comandare via MediaRemote solo l'app "In riproduzione": le altre app
// (Spotify, Music, TV, Brave/Chrome/Edge/Safari) si fermano e ripartono via AppleScript.
// Compilare:  clang -O2 -o fosid fosid.c -framework IOKit -framework CoreFoundation -framework CoreAudio

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hidsystem/IOHIDLib.h> // IOHIDCheckAccess / IOHIDRequestAccess
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <libproc.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FOSI_VID 0x3302 // TTGK Technology (Fosi Audio D2)
#define FOSI_PID 0x33AB

#define MULTI_MS 400 // tempo massimo tra un click e il successivo
#define LONG_MS  600 // durata minima della pressione lunga
#define VOL_STEP (1.0f / 16)

// Usage della pagina HID Consumer inviati dal D2
#define U_PLAYPAUSE 0xCD
#define U_VOICE     0xCF
#define U_VOLUP     0xE9
#define U_VOLDOWN   0xEA

// MediaRemote (framework privato, caricato a runtime)
enum { MR_PLAY = 0, MR_PAUSE = 1, MR_NEXT = 4, MR_PREV = 5 };
static Boolean (*MRSend)(int cmd, CFDictionaryRef opts);

pid_t responsibility_get_pid_responsible_for_pid(pid_t);
extern char **environ;

static CFMutableArrayRef paused; // bundle ID delle app che abbiamo messo in pausa noi

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s  ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *cstr(CFStringRef s, char *buf, size_t n) {
    if (!s || !CFStringGetCString(s, buf, n, kCFStringEncodingUTF8)) snprintf(buf, n, "?");
    return buf;
}

static void after_ms(int ms, dispatch_block_t b) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC), dispatch_get_main_queue(), b);
}

// ---------------------------------------------------------------- media control

enum { PLAYER, SAFARI, CHROMIUM };
static const struct { const char *id; int kind; } scriptable[] = {
    {"com.spotify.client", PLAYER},   {"com.apple.Music", PLAYER},     {"com.apple.TV", PLAYER},
    {"com.apple.Safari", SAFARI},     {"com.brave.Browser", CHROMIUM}, {"com.google.Chrome", CHROMIUM},
    {"com.microsoft.edgemac", CHROMIUM},
};

// Nei browser: ferma i media in riproduzione e li segna, così la pressione lunga fa ripartire solo quelli.
// Una pagina in cui qualcosa sta suonando dimentica i segni vecchi.
#define JS_PAUSE "(()=>{const l=[...document.querySelectorAll('video,audio')].filter(m=>!m.paused);" \
                 "if(!l.length)return;document.querySelectorAll('[data-fosid]').forEach(m=>delete m.dataset.fosid);" \
                 "l.forEach(m=>{m.pause();m.dataset.fosid=1})})()"
#define JS_PLAY  "document.querySelectorAll('[data-fosid]').forEach(m=>{delete m.dataset.fosid;m.play()})"

// Ferma o fa ripartire un'app via osascript (in background). false se l'app non è scriptabile.
static bool script_app(CFStringRef bundle, bool play) {
    char id[256], body[1024], src[2048];
    cstr(bundle, id, sizeof id);
    int kind = -1;
    for (size_t i = 0; i < sizeof scriptable / sizeof *scriptable; i++)
        if (!strcmp(id, scriptable[i].id)) kind = scriptable[i].kind;
    if (kind < 0) return false;

    const char *js = play ? JS_PLAY : JS_PAUSE;
    if (kind == PLAYER) snprintf(body, sizeof body, "%s", play ? "play" : "pause");
    else { // ogni scheda di ogni finestra, senza aspettare risposta: le schede addormentate non rispondono
        char line[768];
        snprintf(line, sizeof line, kind == SAFARI ? "do JavaScript \"%s\" in t" : "execute t javascript \"%s\"", js);
        snprintf(body, sizeof body,
                 "repeat with w in windows\nset ts to tabs of w\nignoring application responses\n"
                 "repeat with t in ts\n%s\nend repeat\nend ignoring\nend repeat",
                 line);
    }
    snprintf(src, sizeof src, "if application id \"%s\" is running then\ntell application id \"%s\"\n%s\nend tell\nend if",
             id, id, body);

    char *argv[] = {"/usr/bin/osascript", "-e", src, NULL};
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ)) logf_("osascript non avviato");
    return true;
}

// Percorso del primo .app che contiene l'eseguibile di pid, o false.
static bool app_path(pid_t pid, char *path) {
    if (proc_pidpath(pid, path, PROC_PIDPATHINFO_MAXSIZE) <= 0) return false;
    if (!strncmp(path, "/Library/Audio/", 15)) return false; // demoni dei driver audio (es. arkaudiod di SoundSource)
    char *app = strstr(path, ".app/");
    if (!app) return false;
    app[4] = 0;
    return true;
}

// Bundle ID dell'app "madre" di un processo (es. Brave Browser Helper -> Brave, WebKit.GPU -> Safari).
// Prima il percorso del processo stesso: l'app principale di Brave/Chrome può girare da una copia
// temporanea "*.app.bundle" che non è un percorso utile.
static CFStringRef app_bundle_id(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (!app_path(pid, path) && !app_path(responsibility_get_pid_responsible_for_pid(pid), path)) return NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), true);
    CFBundleRef b = CFBundleCreate(NULL, url);
    CFRelease(url);
    if (!b) return NULL;
    CFStringRef id = CFBundleGetIdentifier(b);
    if (id) CFRetain(id);
    CFRelease(b);
    return id;
}

// App che in questo momento stanno mandando audio a un'uscita qualsiasi (API CoreAudio, macOS 14+).
static CFMutableArrayRef playing_apps(void) {
    CFMutableArrayRef apps = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyProcessObjectList, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) || !size) return apps;
    AudioObjectID procs[size / sizeof(AudioObjectID)];
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, procs)) return apps;

    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        UInt32 running = 0, sz = sizeof running;
        pid_t pid = 0;
        a.mSelector = kAudioProcessPropertyIsRunningOutput;
        if (AudioObjectGetPropertyData(procs[i], &a, 0, NULL, &sz, &running) || !running) continue;
        a.mSelector = kAudioProcessPropertyPID;
        sz = sizeof pid;
        if (AudioObjectGetPropertyData(procs[i], &a, 0, NULL, &sz, &pid) || pid == getpid()) continue;
        CFStringRef id = app_bundle_id(pid);
        if (!id) continue;
        if (!CFArrayContainsValue(apps, CFRangeMake(0, CFArrayGetCount(apps)), id)) CFArrayAppendValue(apps, id);
        CFRelease(id);
    }
    return apps;
}

// merge=false: la lista "da riprendere" viene sostituita (se c'era qualcosa in riproduzione).
// merge=true:  si aggiunge alla lista (usato per i tentativi ritardati dopo lo scollegamento).
static void pause_all(bool merge) {
    CFMutableArrayRef apps = playing_apps();
    CFIndex n = CFArrayGetCount(apps);
    if (!merge && n > 0) CFArrayRemoveAllValues(paused);

    MRSend(MR_PAUSE, NULL); // l'app "In riproduzione" di sistema
    for (CFIndex i = 0; i < n; i++) {
        CFStringRef id = CFArrayGetValueAtIndex(apps, i);
        char buf[256];
        logf_("pausa -> %s", cstr(id, buf, sizeof buf));
        script_app(id, false);
        if (!CFArrayContainsValue(paused, CFRangeMake(0, CFArrayGetCount(paused)), id)) CFArrayAppendValue(paused, id);
    }
    CFRelease(apps);
}

// Riprende una sola app alla volta, nell'ordine in cui le abbiamo messe in pausa: ogni pressione
// lunga fa ripartire la successiva. Senza niente da riprendere (o per un'app non scriptabile)
// riceve play l'app "In riproduzione".
static void resume_next(void) {
    if (CFArrayGetCount(paused) == 0) {
        MRSend(MR_PLAY, NULL);
        return;
    }
    CFStringRef id = CFRetain(CFArrayGetValueAtIndex(paused, 0));
    CFArrayRemoveValueAtIndex(paused, 0);
    char buf[256];
    logf_("play -> %s (ne restano %ld)", cstr(id, buf, sizeof buf), (long)CFArrayGetCount(paused));
    if (!script_app(id, true)) MRSend(MR_PLAY, NULL);
    CFRelease(id);
}

// Tasti volume del telecomando (se presenti): agiscono sull'uscita di default come i tasti di sistema.
static void volume_step(float delta) {
    AudioObjectID dev = 0;
    UInt32 sz = sizeof dev;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev)) return;
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, 0};
    for (a.mElement = 0; a.mElement <= 2; a.mElement++) { // master, altrimenti canali L/R
        Float32 v;
        sz = sizeof v;
        if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &v)) continue;
        v = fminf(1, fmaxf(0, v + delta));
        AudioObjectSetPropertyData(dev, &a, 0, NULL, sz, &v);
        if (a.mElement == 0) break;
    }
}

// ---------------------------------------------------------------- gesti del tasto

static int clicks, seq;
static bool down, long_fired;

static void clicks_done(int n) {
    logf_("%d click", n);
    if (n == 1) pause_all(false);
    else if (n == 2) MRSend(MR_NEXT, NULL);
    else MRSend(MR_PREV, NULL);
}

static void on_button(bool pressed) {
    int my = ++seq; // ogni evento invalida i timer precedenti
    if (pressed) {
        down = true;
        long_fired = false;
        after_ms(LONG_MS, ^{
            if (my != seq || !down) return;
            long_fired = true;
            clicks = 0;
            logf_("pressione lunga");
            resume_next();
        });
    } else {
        down = false;
        if (long_fired) return;
        clicks++;
        after_ms(MULTI_MS, ^{
            if (my != seq) return;
            int n = clicks;
            clicks = 0;
            clicks_done(n);
        });
    }
}

// ---------------------------------------------------------------- HID

static void on_value(void *ctx, IOReturn r, void *sender, IOHIDValueRef value) {
    static bool state[4]; // stato precedente di CD, CF, E9, EA: reagiamo solo ai cambi
    IOHIDElementRef el = IOHIDValueGetElement(value);
    if (IOHIDElementGetUsagePage(el) != kHIDPage_Consumer) return;
    uint32_t usage = IOHIDElementGetUsage(el);
    bool on = IOHIDValueGetIntegerValue(value) != 0;
    int i = usage == U_PLAYPAUSE ? 0 : usage == U_VOICE ? 1 : usage == U_VOLUP ? 2 : usage == U_VOLDOWN ? 3 : -1;
    if (i < 0 || state[i] == on) return;
    state[i] = on;

    switch (usage) {
    case U_PLAYPAUSE: on_button(on); break;
    case U_VOICE: // alcuni firmware mandano "Voice Command" sulla pressione lunga
        if (on) {
            logf_("voice command");
            seq++, clicks = 0;
            resume_next();
        }
        break;
    case U_VOLUP:   if (on) volume_step(+VOL_STEP); break;
    case U_VOLDOWN: if (on) volume_step(-VOL_STEP); break;
    }
}

static void on_attach(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef dev) {
    IOReturn rc = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
    if (rc != kIOReturnSuccess) {
        // Di solito manca il permesso "Monitoraggio input": usciamo e launchd ci riprova tra poco.
        logf_("impossibile prendere il Fosi in esclusiva (0x%x): concedi 'Monitoraggio input' a fosid", rc);
        exit(1);
    }
    IOHIDDeviceRegisterInputValueCallback(dev, on_value, NULL);
    logf_("Fosi collegato");
}

static void on_detach(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef dev) {
    logf_("Fosi scollegato: pausa di tutto");
    seq++, clicks = 0, down = false;
    pause_all(true);
    // Le app ci mettono un attimo a spostarsi sugli altoparlanti: riproviamo per beccarle.
    after_ms(500, ^{ pause_all(true); });
    after_ms(1500, ^{ pause_all(true); });
}

int main(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);

    void *mr = dlopen("/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote", RTLD_LAZY);
    MRSend = mr ? dlsym(mr, "MRMediaRemoteSendCommand") : NULL;
    if (!MRSend) {
        logf_("MediaRemote non disponibile");
        return 1;
    }
    signal(SIGCHLD, SIG_IGN); // i processi osascript finiti si ripuliscono da soli

    if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) {
        logf_("chiedo il permesso 'Monitoraggio input'");
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    }

    paused = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

    IOHIDManagerRef mgr = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    int vid = FOSI_VID, pid = FOSI_PID, page = kHIDPage_Consumer, usage = kHIDUsage_Csmr_ConsumerControl;
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey), CFSTR(kIOHIDPrimaryUsagePageKey),
                          CFSTR(kIOHIDPrimaryUsageKey)};
    const void *vals[] = {CFNumberCreate(NULL, kCFNumberIntType, &vid), CFNumberCreate(NULL, kCFNumberIntType, &pid),
                          CFNumberCreate(NULL, kCFNumberIntType, &page), CFNumberCreate(NULL, kCFNumberIntType, &usage)};
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, match);
    IOHIDManagerRegisterDeviceMatchingCallback(mgr, on_attach, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, on_detach, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    logf_("fosid avviato");
    CFRunLoopRun();
    return 0;
}
