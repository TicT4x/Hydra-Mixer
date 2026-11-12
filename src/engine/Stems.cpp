#include "Stems.h"
#include "core/StemDsp.h"

#include <windows.h>
#include <bcrypt.h>
#if HYDRA_STEMS
 #include <dxgi1_6.h>
 #include <d3d12.h>
 #include <dml_provider_factory.h>
 #pragma comment (lib, "dxgi.lib")
#endif
#include <mutex>

namespace hydra::stems
{
juce::String stemName (int s)
{
    static const char* const names[] = { "Vocals", "Drums", "Bass", "Guitar", "Rest" };
    return juce::isPositiveAndBelow (s, (int) numStems) ? names[s] : "";
}

const ModelInfo& modelInfo (Model m)
{
    static const ModelInfo models[] = {
        { "BS-RoFormer SW, 6 stems", "BS-RoFormer-SW-6stem.hydra.onnx",
          "https://huggingface.co/elicwhite/bs-roformer-sw-6stem-onnx/resolve/main/bs_roformer_sw_6stem_fp16.onnx",
          "d3d2bac77a7023282cb5f35a5807179e34076b60589867b572275f1a8ec36444", 352778874, 176400, true,
          "The model weights have no stated license (community model): for personal use." },
        // Folded (12.11.): the export computes its shapes at run time (~25 000 nodes); folded once, a session is
        // ready in ~2 s instead of ~7 s, with bit-identical output. BS-RoFormer gained only ~1 s for +350 MB on disk.
        { "HT-Demucs, 4 stems (no guitar)", "HTDemucs-4stem.folded.onnx",
          "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/htdemucs_fp16weights.onnx",
          "d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a", 165612636, 343980, false,
          "MIT license (Meta's HT-Demucs).", "HTDemucs-4stem.hydra.onnx" } };
    return models[juce::jlimit (0, kNumModels - 1, (int) m)];
}

juce::StringArray modeNames() { return { "Fast", "Balanced", "Best", "Live" }; }
Model modelFor (int mode) noexcept { return mode == kLiveMode ? Model::demucs : Model::roformer; }

Timing timingFor (int mode) noexcept
{
    // BS-RoFormer: window 4 s = left context + hop + fade + look-ahead (Best: the played part in the middle).
    // Budget: time a worker may take for one window (RTX 5060, 10.11.: BS-RoFormer ~0.55 s, HT-Demucs ~0.2 s).
    Timing t;
    switch (juce::jlimit (0, kNumModes - 1, mode))
    {
        case 0:  t.lookAhead = 0.3;   t.budget = 0.65; break;
        case 1:  t.lookAhead = 0.75;  t.budget = 0.8;  break;
        case 2:  t.lookAhead = 1.375; t.budget = 1.0;  break;
        default: t.hop = 0.6; t.fade = 0.1; t.lookAhead = 0.2; t.budget = 0.4; break;   // Live (HT-Demucs, 7.8-s window)
    }
    return t;
}

double typicalMs (Model m) noexcept { return m == Model::roformer ? 550.0 : 140.0; }

Timing timingFor (int mode, const Tuning& tuning, double modelMs) noexcept
{
    if (tuning.automatic) return timingFor (mode);
    // One window per hop: GPU share = time per window / hop. A job (model + resampling, ~40 ms more on the CPU) must
    // also fit into a hop and into the budget. Less look-ahead in Live, where latency is the point.
    const double c = (modelMs > 0.0 ? modelMs : typicalMs (modelFor (mode))) / 1000.0;
    const double job = c + 0.04;
    Timing t;
    t.hop = juce::jmax (c / juce::jlimit (0.1, 1.0, (double) tuning.share), job * 1.1, 0.05);
    t.fade = juce::jlimit (0.02, 0.25, t.hop * 0.2);
    t.lookAhead = mode == kLiveMode ? 0.1 : timingFor (mode).lookAhead;
    t.budget = job * 1.3 + 0.03;
    // The played part and its look-ahead must fit into the window (BS-RoFormer: 4 s), with some left context.
    const double window = (double) modelInfo (modelFor (mode)).samples / Separator::kRate;
    t.hop = juce::jmin (t.hop, window * 0.6);
    t.fade = juce::jlimit (0.02, 0.25, t.hop * 0.2);
    if (t.hop + t.fade + t.lookAhead > window * 0.9) t.lookAhead = juce::jmax (0.05, window * 0.9 - t.hop - t.fade);
    return t;
}

namespace
{
/** Samples of a timing at a sample rate (fade even, so its middle is a whole sample). */
struct Frames
{
    int64_t hop = 0, fade = 0, lookAhead = 0, budget = 0;
    Frames() = default;
    Frames (const Timing& t, double fs)
        : hop ((int64_t) std::llround (t.hop * fs)), fade (2 * (int64_t) std::llround (t.fade * fs * 0.5)),
          lookAhead ((int64_t) std::llround (t.lookAhead * fs)), budget ((int64_t) std::llround (t.budget * fs)) {}
    int64_t latency (int hwIn) const noexcept { return hop + fade + lookAhead + budget + hwIn; }
};

Frames framesOf (const Instance& inst)
{
    const auto a = inst.getFrames();
    Frames f;
    f.hop = a[0]; f.fade = a[1]; f.lookAhead = a[2]; f.budget = a[3];
    return f;
}

/** Input samples the resampler to 44.1 kHz needs beyond a window (0 at 44.1 kHz). */
int inputHalfWidth (double fs) { return fs == (double) Separator::kRate ? 0 : Resampler (fs / Separator::kRate).halfWidth(); }

// The longest a stream may be late (sizes the rings): Best in Auto, or a slow manual setting (10 % GPU).
double longestLatency()
{
    double l = 0.0;
    for (int m = 0; m < kNumModes; ++m)
    {
        l = juce::jmax (l, timingFor (m).latency());
        l = juce::jmax (l, timingFor (m, { false, 0.1f }, typicalMs (modelFor (m)) * 1.5).latency());
    }
    return l;
}
double longestWindow()  { return (double) juce::jmax (modelInfo (Model::roformer).samples, modelInfo (Model::demucs).samples) / Separator::kRate; }

bool sha256 (const juce::File& file, juce::String& hex, const std::atomic<bool>& cancel)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = BCryptCreateHash (alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    juce::FileInputStream in (file);
    ok = ok && in.openedOk();
    std::vector<char> chunk (1 << 20);
    while (ok && ! in.isExhausted() && ! cancel.load())
    {
        const int n = in.read (chunk.data(), (int) chunk.size());
        if (n <= 0) break;
        ok = BCryptHashData (hash, (PUCHAR) chunk.data(), (ULONG) n, 0) == 0;
    }
    uint8_t digest[32] {};
    ok = ok && ! cancel.load() && BCryptFinishHash (hash, digest, 32, 0) == 0;
    if (hash != nullptr) BCryptDestroyHash (hash);
    BCryptCloseAlgorithmProvider (alg, 0);
    if (ok) hex = juce::String::toHexString (digest, 32, 0);
    return ok;
}
} // namespace

//==============================================================================
#if HYDRA_STEMS
juce::File extractRuntime (const juce::File& base, juce::String& error)
{
    // Resource ids: CMakeLists.txt (hydra_ort.rc).
    struct Part { int id; const char* name; };
    static constexpr Part parts[] = { { 201, "onnxruntime.dll" }, { 202, "DirectML.dll" } };
    auto resource = [] (int id, DWORD& size) -> const void*
    {
        HRSRC r = FindResourceW (nullptr, MAKEINTRESOURCEW (id), MAKEINTRESOURCEW (10));   // RT_RCDATA
        HGLOBAL g = r != nullptr ? LoadResource (nullptr, r) : nullptr;
        size = r != nullptr ? SizeofResource (nullptr, r) : 0;
        return g != nullptr ? LockResource (g) : nullptr;
    };
    DWORD ortSize = 0;
    if (resource (201, ortSize) == nullptr || ortSize == 0) return {};   // not embedded in this exe
    // One folder per runtime: a running older Hydra keeps its DLLs loaded (locked) without being in the way.
    const auto dir = base.getChildFile ("ort-" + juce::String ((juce::int64) ortSize));
    if (! dir.createDirectory()) { error = "could not create " + dir.getFullPathName(); return {}; }
    for (const auto& p : parts)
    {
        DWORD size = 0;
        const void* data = resource (p.id, size);
        if (data == nullptr) { error = juce::String (p.name) + " is missing in the program"; return {}; }
        const auto file = dir.getChildFile (p.name);
        if (file.getSize() == (juce::int64) size) continue;   // already unpacked
        const auto tmp = dir.getChildFile (juce::String (p.name) + ".tmp");
        tmp.deleteFile();
        bool ok = false;
        {
            juce::FileOutputStream out (tmp);
            ok = out.openedOk() && out.write (data, size);
            out.flush();
            ok = ok && ! out.getStatus().failed();
        }
        if (! ok || ! tmp.moveFileTo (file))
        {
            tmp.deleteFile();
            error = "could not unpack " + juce::String (p.name) + " to " + dir.getFullPathName();
            return {};
        }
    }
    return dir;
}

namespace
{
/** onnxruntime.dll, loaded on first use (Windows has an older one in System32: the full path makes sure ours is
    taken, LOAD_WITH_ALTERED_SEARCH_PATH finds DirectML.dll next to it too). Taken from the exe itself (see
    extractRuntime), otherwise from next to the exe. */
const OrtApi* ortApi (juce::String& error)
{
    static const OrtApi* api = nullptr;
    static juce::String loadError;
    static std::once_flag once;
    std::call_once (once, []
    {
        auto dll = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("onnxruntime.dll");
        juce::String extractError;
        if (const auto dir = extractRuntime (appDataDir().getChildFile ("Runtime"), extractError); dir != juce::File())
            dll = dir.getChildFile ("onnxruntime.dll");
        if (! dll.existsAsFile())
        {
            loadError = extractError.isNotEmpty() ? extractError : juce::String ("onnxruntime.dll is missing (this build has no ONNX Runtime)");
            return;
        }
        HMODULE h = LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h == nullptr) { loadError = "onnxruntime.dll could not be loaded (error " + juce::String ((int) GetLastError()) + ")"; return; }
        using GetBase = const OrtApiBase* (ORT_API_CALL*) ();
        auto get = (GetBase) (void*) GetProcAddress (h, "OrtGetApiBase");
        const OrtApiBase* base = get != nullptr ? get() : nullptr;
        api = base != nullptr ? base->GetApi (ORT_API_VERSION) : nullptr;
        if (api == nullptr) loadError = "onnxruntime.dll is too old (needs API version " + juce::String (ORT_API_VERSION) + ")";
    });
    error = loadError;
    return api;
}

juce::String gpuName()
{
    IDXGIFactory6* factory = nullptr;
    juce::String name;
    if (SUCCEEDED (CreateDXGIFactory1 (__uuidof (IDXGIFactory6), (void**) &factory)))
    {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED (factory->EnumAdapterByGpuPreference (0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof (IDXGIAdapter1), (void**) &adapter)))
        {
            DXGI_ADAPTER_DESC1 d {};
            if (SUCCEEDED (adapter->GetDesc1 (&d))) name = juce::String (d.Description);
            adapter->Release();
        }
        factory->Release();
    }
    return name;
}

/** Name of the first dedicated (discrete) graphics card, empty if there is none. Integrated graphics share the
    system memory and the CPU's power budget: Stems would slow down the audio engine there and still not keep up
    (wish of the user 11.11.: do not load it at all, say why). A card is dedicated if Direct3D 12 says it has no
    unified memory (UMA); without Direct3D 12, if it has at least 1 GB of its own memory. Software adapters
    (Microsoft Basic Render) never count. Checked once (~0.1 s), any thread. */
juce::String dedicatedGpuName()
{
    static std::once_flag once;
    static juce::String found;
    std::call_once (once, []
    {
        using CreateDevice = HRESULT (WINAPI*) (IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        HMODULE d3d = LoadLibraryExW (L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto create = d3d != nullptr ? (CreateDevice) (void*) GetProcAddress (d3d, "D3D12CreateDevice") : nullptr;
        IDXGIFactory6* factory = nullptr;
        if (FAILED (CreateDXGIFactory1 (__uuidof (IDXGIFactory6), (void**) &factory))) return;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; found.isEmpty()
                         && SUCCEEDED (factory->EnumAdapterByGpuPreference (i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof (IDXGIAdapter1), (void**) &adapter));
             ++i)
        {
            DXGI_ADAPTER_DESC1 d {};
            const bool hardware = SUCCEEDED (adapter->GetDesc1 (&d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && d.VendorId != 0x1414;
            bool dedicated = false;
            if (hardware)
            {
                ID3D12Device* dev = nullptr;
                if (create != nullptr && SUCCEEDED (create (adapter, D3D_FEATURE_LEVEL_11_0, __uuidof (ID3D12Device), (void**) &dev)) && dev != nullptr)
                {
                    D3D12_FEATURE_DATA_ARCHITECTURE arch {};
                    dedicated = SUCCEEDED (dev->CheckFeatureSupport (D3D12_FEATURE_ARCHITECTURE, &arch, sizeof (arch))) && ! arch.UMA;
                    dev->Release();
                }
                else
                    dedicated = d.DedicatedVideoMemory >= ((SIZE_T) 1 << 30);
            }
            if (dedicated) found = juce::String (d.Description);
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
    });
    return found;
}

/** An ONNX Runtime session (DirectML on the high-performance GPU, or CPU) running float tensors. */
class OrtSessionRunner
{
public:
    ~OrtSessionRunner()
    {
        if (api == nullptr) return;
        if (session != nullptr) api->ReleaseSession (session);
        if (memInfo != nullptr) api->ReleaseMemoryInfo (memInfo);
        if (env != nullptr) api->ReleaseEnv (env);
    }

    bool open (const juce::File& model, bool cpu, juce::String& error)
    {
        api = ortApi (error);
        if (api == nullptr) return false;
        auto check = [&] (OrtStatus* s) { return ok (s, error); };
        OrtSessionOptions* opts = nullptr;
        bool good = check (api->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "hydra-stems", &env))
                 && check (api->CreateSessionOptions (&opts))
                 && check (api->SetSessionGraphOptimizationLevel (opts, ORT_ENABLE_ALL))
                 // ONNX Runtime starts one CPU thread per core that busy-waits between runs (spinning), at Hydra's
                 // high process priority. With DirectML the GPU does the work; spinning threads only take CPU time
                 // from the audio engine and the stream threads (clicks under load). A quarter of the cores (2-4)
                 // for the few operators that stay on the CPU, no spinning.
                 && check (api->SetIntraOpNumThreads (opts, cpu ? 0 : juce::jlimit (2, 4, juce::SystemStats::getNumCpus() / 4)))
                 && check (api->SetInterOpNumThreads (opts, 1))
                 && check (api->AddSessionConfigEntry (opts, "session.intra_op.allow_spinning", "0"))
                 && check (api->AddSessionConfigEntry (opts, "session.inter_op.allow_spinning", "0"));
        if (good && ! cpu)
        {
            const OrtDmlApi* dml = nullptr;
            good = check (api->DisableMemPattern (opts))                     // required by the DirectML provider
                && check (api->SetSessionExecutionMode (opts, ORT_SEQUENTIAL))
                && check (api->GetExecutionProviderApi ("DML", ORT_API_VERSION, (const void**) &dml)) && dml != nullptr;
            OrtDmlDeviceOptions dev { HighPerformance, Gpu };
            if (good) good = check (dml->SessionOptionsAppendExecutionProvider_DML2 (opts, &dev));
            if (! good) error = "DirectX 12 GPU needed (" + error + ")";
        }
        if (good) good = check (api->CreateSession (env, model.getFullPathName().toWideCharPointer(), opts, &session));
        if (opts != nullptr) api->ReleaseSessionOptions (opts);
        return good && check (api->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &memInfo));
    }

    struct Tensor { const char* name; const int64_t* shape; size_t rank; float* data; size_t count; };
    bool run (const std::vector<Tensor>& ins, const std::vector<Tensor>& outs, juce::String& error)
    {
        std::vector<OrtValue*> inV, outV;
        std::vector<const char*> inN, outN;
        bool good = true;
        auto make = [&] (const Tensor& t, std::vector<OrtValue*>& to)
        {
            OrtValue* v = nullptr;
            good = good && ok (api->CreateTensorWithDataAsOrtValue (memInfo, t.data, t.count * sizeof (float), t.shape, t.rank,
                                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v), error);
            to.push_back (v);
        };
        for (auto& t : ins)  { make (t, inV);  inN.push_back (t.name); }
        for (auto& t : outs) { make (t, outV); outN.push_back (t.name); }
        if (good)
            good = ok (api->Run (session, nullptr, inN.data(), (const OrtValue* const*) inV.data(), inV.size(), outN.data(), outV.size(), outV.data()), error);
        for (auto* v : inV)  if (v != nullptr) api->ReleaseValue (v);
        for (auto* v : outV) if (v != nullptr) api->ReleaseValue (v);
        return good;
    }

private:
    bool ok (OrtStatus* s, juce::String& error)
    {
        if (s == nullptr) return true;
        error = api->GetErrorMessage (s);
        api->ReleaseStatus (s);
        return false;
    }
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memInfo = nullptr;
};

/** BS-RoFormer SW (STFT outside the graph): spec_real / spec_imag [1, 2, 1025, 345] -> out_spec_* [1, 6, 2, 1025, 345]. */
class RoformerSeparator final : public Separator
{
public:
    static constexpr int kFrames = 345, kBins = Stft::kBins;
    static constexpr int kModelIndex[kModelStems] = { 3, 1, 0, 4 };   // model order: bass, drums, other, vocals, guitar, piano

    bool open (const juce::File& f)
    {
        const size_t inCount = 2 * (size_t) kBins * kFrames;
        specRe.resize (inCount); specIm.resize (inCount); outRe.resize (6 * inCount); outIm.resize (6 * inCount);
        return runner.open (f, false, lastError);
    }
    int samples() const override { return modelInfo (Model::roformer).samples; }
    bool separate (const float* const* in, float* const* out) override
    {
        const int n = samples();
        const size_t plane = (size_t) kBins * kFrames;
        for (int ch = 0; ch < 2; ++ch) stft.forward (in[ch], n, specRe.data() + ch * plane, specIm.data() + ch * plane);
        static const int64_t inShape[] = { 1, 2, kBins, kFrames }, outShape[] = { 1, 6, 2, kBins, kFrames };
        if (! runner.run ({ { "spec_real", inShape, 4, specRe.data(), specRe.size() }, { "spec_imag", inShape, 4, specIm.data(), specIm.size() } },
                          { { "out_spec_real", outShape, 5, outRe.data(), outRe.size() }, { "out_spec_imag", outShape, 5, outIm.data(), outIm.size() } },
                          lastError))
            return false;
        for (int s = 0; s < kModelStems; ++s)
            for (int ch = 0; ch < 2; ++ch)
            {
                const size_t at = ((size_t) kModelIndex[s] * 2 + (size_t) ch) * plane;
                stft.inverse (outRe.data() + at, outIm.data() + at, kFrames, out[s * 2 + ch], n);
            }
        return true;
    }

private:
    OrtSessionRunner runner;
    std::vector<float> specRe, specIm, outRe, outIm;
    Stft stft;
};

/** HT-Demucs (waveform in and out): mix [1, 2, 343980] -> stems [1, 4, 2, 343980] (drums, bass, other, vocals). */
class DemucsSeparator final : public Separator
{
public:
    bool open (const juce::File& f)
    {
        mix.resize (2 * (size_t) samples());
        stems.resize (8 * (size_t) samples());
        return runner.open (f, false, lastError);
    }
    int samples() const override { return modelInfo (Model::demucs).samples; }
    bool separate (const float* const* in, float* const* out) override
    {
        const int n = samples();
        for (int ch = 0; ch < 2; ++ch) std::memcpy (mix.data() + (size_t) ch * n, in[ch], sizeof (float) * (size_t) n);
        const int64_t inShape[] = { 1, 2, n }, outShape[] = { 1, 4, 2, n };
        if (! runner.run ({ { "mix", inShape, 3, mix.data(), mix.size() } }, { { "stems", outShape, 4, stems.data(), stems.size() } }, lastError))
            return false;
        static constexpr int kModelIndex[3] = { 3, 0, 1 };   // vocals, drums, bass
        for (int s = 0; s < kModelStems; ++s)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* dst = out[s * 2 + ch];
                if (s == guitar) std::fill (dst, dst + n, 0.0f);   // no guitar: it stays in the rest
                else std::memcpy (dst, stems.data() + ((size_t) kModelIndex[s] * 2 + (size_t) ch) * (size_t) n, sizeof (float) * (size_t) n);
            }
        return true;
    }

private:
    OrtSessionRunner runner;
    std::vector<float> mix, stems;
};
} // namespace
#endif

bool runOnnxForTests (const juce::File& model, bool cpu, const std::vector<TensorData>& inputs, std::vector<TensorData>& outputs,
                      juce::String& error)
{
#if HYDRA_STEMS
    OrtSessionRunner r;
    if (! r.open (model, cpu, error)) return false;
    std::vector<OrtSessionRunner::Tensor> ins, outs;
    auto ins2 = inputs;
    for (auto& t : ins2) ins.push_back ({ t.name, t.shape.data(), t.shape.size(), t.data.data(), t.data.size() });
    for (auto& t : outputs)
    {
        size_t count = 1;
        for (auto d : t.shape) count *= (size_t) d;
        t.data.assign (count, 0.0f);
        outs.push_back ({ t.name, t.shape.data(), t.shape.size(), t.data.data(), t.data.size() });
    }
    return r.run (ins, outs, error);
#else
    juce::ignoreUnused (model, cpu, inputs, outputs);
    error = "built without ONNX Runtime";
    return false;
#endif
}

bool foldModel (const juce::File& in, const juce::File& out, juce::String& error)
{
#if HYDRA_STEMS
    const OrtApi* api = ortApi (error);
    if (api == nullptr) return false;
    auto ok = [&] (OrtStatus* s)
    {
        if (s == nullptr) return true;
        error = api->GetErrorMessage (s);
        api->ReleaseStatus (s);
        return false;
    };
    const auto tmp = out.getSiblingFile (out.getFileName() + ".tmp");
    tmp.deleteFile();
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    // A CPU session with the basic level: ONNX Runtime documents these as provider independent (applied before the
    // graph is split between providers), so the saved model is plain ONNX and runs the same on DirectML.
    bool good = ok (api->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "hydra-fold", &env))
             && ok (api->CreateSessionOptions (&opts))
             && ok (api->SetSessionGraphOptimizationLevel (opts, ORT_ENABLE_BASIC))
             && ok (api->SetIntraOpNumThreads (opts, 1))
             && ok (api->AddSessionConfigEntry (opts, "session.intra_op.allow_spinning", "0"))
             && ok (api->SetOptimizedModelFilePath (opts, tmp.getFullPathName().toWideCharPointer()))
             && ok (api->CreateSession (env, in.getFullPathName().toWideCharPointer(), opts, &session));
    if (session != nullptr) api->ReleaseSession (session);
    if (opts != nullptr) api->ReleaseSessionOptions (opts);
    if (env != nullptr) api->ReleaseEnv (env);
    if (good && tmp.getSize() < in.getSize() / 2) { good = false; error = "the folded model was not written"; }
    if (good && ! tmp.moveFileTo (out)) { good = false; error = "cannot write " + out.getFullPathName(); }
    if (! good) tmp.deleteFile();
    return good;
#else
    juce::ignoreUnused (in, out);
    error = "built without ONNX Runtime";
    return false;
#endif
}

//==============================================================================
Instance::Instance() = default;
Instance::~Instance() = default;

void Instance::prepare (double fs)
{
    const std::lock_guard<std::mutex> lock (workLock);
    sampleRate = fs;
    hwIn = inputHalfWidth (fs);
    // Input ring (also the dry delay line): the longest window + the longest latency + room. Stems: the latency + room.
    inSize = juce::nextPowerOfTwo ((int) std::ceil ((longestWindow() + longestLatency() + 1.0) * fs) + 2 * kMaxBlock);
    stemSize = juce::nextPowerOfTwo ((int) std::ceil ((longestLatency() + 2.0) * fs) + 2 * kMaxBlock);
    inMask = inSize - 1;
    stemMask = stemSize - 1;
    inBuf.assign (2 * (size_t) inSize, 0.0f);
    stemBuf.assign ((size_t) kStemChannels * (size_t) stemSize, 0.0f);
    written = 0;
    mode = -1;        // the first block starts a stream
    wasReady = false;
    wetLast = 0.0f;
    lastMix.fill (StemMix::gain (1.0f));
    latencyNow = 0;
    latency.store (0);
    writtenShared.store (0);
    ready.store (0);
    validFrom.store (0);
    readyEpoch.store (~0u);
    workEpoch = ~0u;
}

void Instance::restartStream (int m, const Tuning& tuning) noexcept
{
    epoch = (epoch + 1) & 0xffffffu;
    // "Changing" (mode 0) while the timing of the new epoch is written: a worker reading now retries.
    epochInfo.store (epoch << 8, std::memory_order_release);
    mode = m;
    tuningNow = tuningPending = tuning;
    pendingSince = 0;
    const Timing t = timingFor (m, tuning, Service::referenceMs (modelFor (m)));
    const Frames f (t, sampleRate);
    hopF.store (f.hop, std::memory_order_relaxed);
    fadeF.store (f.fade, std::memory_order_relaxed);
    lookF.store (f.lookAhead, std::memory_order_relaxed);
    budgetF.store (f.budget, std::memory_order_relaxed);
    hopSeconds.store (t.hop, std::memory_order_relaxed);
    latencyNow = wasReady ? (int) f.latency (hwIn) : 0;
    latency.store (latencyNow, std::memory_order_relaxed);
    written = 0;
    writtenShared.store (0, std::memory_order_release);   // before the new epoch (the worker reads in the other order)
    epochInfo.store (epoch << 8 | (uint32_t) (m + 1), std::memory_order_release);
    wetLast = 0.0f;
    wetShown.store (false, std::memory_order_relaxed);
    everWet.store (false, std::memory_order_relaxed);
}

void Instance::process (float* l, float* r, int n, const float* gains, int m, bool restart, const Tuning& tuning,
                        std::atomic<float>* peaks, int readers) noexcept
{
    std::array<StemMix, numStems> mix;
    for (int s = 0; s < numStems; ++s) mix[(size_t) s] = StemMix::gain (gains[s]);
    process (l, r, n, mix.data(), m, restart, tuning, peaks, readers);
}

void Instance::process (float* l, float* r, int n, const StemMix* mixIn, int m, bool restart, const Tuning& tuning,
                        std::atomic<float>* peaks, int readers) noexcept
{
    if (inBuf.empty()) return;
    blocks.store (blocks.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);   // only this thread writes
    m = juce::jlimit (0, kNumModes - 1, m);
    const bool on = Service::isReady (modelFor (m));
    // A new GPU setting is taken once it has stayed for 0.4 s (dragging the slider would start over all the time).
    bool retune = false;
    if (! (tuning == tuningPending)) { tuningPending = tuning; pendingSince = 0; }
    else if (! (tuningPending == tuningNow) && (pendingSince += n) >= (int64_t) (0.4 * sampleRate)) retune = true;
    if (restart || m != mode || on != wasReady || retune)
    {
        wasReady = on;
        restartStream (m, tuningPending);
    }
    if (! on) return;   // no model (yet): the channel passes unchanged, without latency

    float* inL = in (0);
    float* inR = in (1);
    for (int i = 0; i < n; ++i)
    {
        const auto at = (size_t) ((written + i) & inMask);
        inL[at] = l[i];
        inR[at] = r[i];
    }
    const int64_t s0 = written - latencyNow;   // stream position played now
    written += n;
    writtenShared.store (written, std::memory_order_release);

    // Stems ready for the whole block? Otherwise the dry signal (crossfaded in ~20 ms).
    const bool sameEpoch = readyEpoch.load (std::memory_order_acquire) == epoch;
    const bool cover = sameEpoch && s0 >= validFrom.load (std::memory_order_relaxed) && s0 + n <= ready.load (std::memory_order_relaxed);
    if (! cover && s0 >= 0 && wetShown.load (std::memory_order_relaxed)) missed.fetch_add ((uint64_t) n, std::memory_order_relaxed);
    const float maxStep = (float) n / (float) (0.02 * sampleRate);
    const float wetNext = cover ? juce::jmin (1.0f, wetLast + maxStep) : juce::jmax (0.0f, wetLast - maxStep);
    const bool metering = cover && peaks != nullptr;

    std::array<StemMix, numStems> target {};
    for (int s = 0; s < numStems; ++s) target[(size_t) s] = mixIn[s];
    if (wetLast == 0.0f && wetNext == 0.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = s0 + i;
            const auto at = (size_t) (p & inMask);
            l[i] = p >= 0 ? inL[at] : 0.0f;
            r[i] = p >= 0 ? inR[at] : 0.0f;
        }
    }
    else
    {
        const float* st[kStemChannels];
        for (int c = 0; c < kStemChannels; ++c) st[c] = stem (c);
        std::array<float, numStems> pk {};
        const float inv = 1.0f / (float) n;
        for (int i = 0; i < n; ++i)
        {
            const int64_t p = s0 + i;
            const auto at = (size_t) (p & inMask), sat = (size_t) (p & stemMask);
            const float t = (float) (i + 1) * inv;
            const float wet = wetLast + (wetNext - wetLast) * t;
            // Each stem's 2x2 mix glides over the block. The rest is the input minus the model's stems, so
            // out = Rest * input + sum (Stem_s - Rest) * stem_s.
            StemMix g[numStems];
            for (int s = 0; s < numStems; ++s)
            {
                const auto& a = lastMix[(size_t) s];
                const auto& b = target[(size_t) s];
                g[s] = { a.ll + (b.ll - a.ll) * t, a.lr + (b.lr - a.lr) * t, a.rl + (b.rl - a.rl) * t, a.rr + (b.rr - a.rr) * t };
            }
            const float dl = p >= 0 ? inL[at] : 0.0f, dr = p >= 0 ? inR[at] : 0.0f;
            const auto& R = g[rest];
            float ml = R.ll * dl + R.lr * dr, mr = R.rl * dl + R.rr * dr, restL = dl, restR = dr;
            for (int s = 0; s < kModelStems; ++s)
            {
                const float sl = st[2 * s][sat], sr = st[2 * s + 1][sat];
                ml += (g[s].ll - R.ll) * sl + (g[s].lr - R.lr) * sr;
                mr += (g[s].rl - R.rl) * sl + (g[s].rr - R.rr) * sr;
                if (metering)
                {
                    pk[(size_t) s] = juce::jmax (pk[(size_t) s], std::abs (sl), std::abs (sr));
                    restL -= sl;
                    restR -= sr;
                }
            }
            if (metering) pk[rest] = juce::jmax (pk[rest], std::abs (restL), std::abs (restR));
            l[i] = dl + wet * (ml - dl);
            r[i] = dr + wet * (mr - dr);
        }
        if (metering)
            for (int rd = 0; rd < readers; ++rd)
                for (int s = 0; s < numStems; ++s)
                    if (auto& a = peaks[rd * numStems + s]; pk[(size_t) s] > a.load (std::memory_order_relaxed))
                        a.store (pk[(size_t) s], std::memory_order_relaxed);
    }
    lastMix = target;
    wetLast = wetNext;
    wetShown.store (wetNext > 0.0f, std::memory_order_relaxed);
    if (wetNext > 0.0f && ! everWet.load (std::memory_order_relaxed)) everWet.store (true, std::memory_order_relaxed);
}

//==============================================================================
/** One model: its worker thread, the loaded separator, its state. */
struct Service::Slot
{
    Model id = Model::roformer;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<Separator> separator, testSeparator;
    juce::File testModel;
    Status status;                                       // under Service::statusLock
    std::atomic<double> avgMs { 0.0 };
    int loadFailures = 0;
    bool foldFailed = false;   // folding failed once: load the unfolded file (slower) until the next start
    std::atomic<bool> wantDownload { false }, cancel { false }, wantUnload { false };
    double idleSince = -1.0;   // ms: loaded but not in use since then (-1: in use)
    // The last model runs (end time, duration in ms) for the measured GPU load.
    static constexpr int kRuns = 64;
    mutable std::mutex runsLock;
    std::array<std::pair<double, double>, kRuns> runs {};
    int runPos = 0;
    double runStart = -1.0;   // under runsLock: start of the model run in progress (-1: none)
    // Worker scratch.
    std::vector<float> win[2], stemOut[kModelStems][2], gather, back;
    double ratesFs = 0.0;
    std::unique_ptr<Resampler> toModel, fromModel;
};

class Service::Worker final : public juce::Thread
{
public:
    Worker (Service& s, Slot& sl) : juce::Thread (juce::String ("Hydra Stems ") + modelInfo (sl.id).name), service (s), slot (sl) {}
    void run() override { service.run (slot); }
    Service& service;
    Slot& slot;
};

static std::unique_ptr<Service>& serviceHolder()
{
    static std::unique_ptr<Service> s;
    return s;
}

Service& Service::get()
{
    static std::mutex m;
    const std::lock_guard<std::mutex> lock (m);
    auto& s = serviceHolder();
    if (s == nullptr) s.reset (new Service());
    return *s;
}

void Service::shutdown()
{
    auto& s = serviceHolder();
    if (s == nullptr) return;
    for (auto& sl : s->slots) if (sl->worker != nullptr) sl->worker->stopThread (10000);
    s.reset();
}

Service::Service()
{
    for (int m = 0; m < kNumModels; ++m)
    {
        auto sl = std::make_unique<Slot>();
        sl->id = (Model) m;
#if ! HYDRA_STEMS
        sl->status.state = State::unavailable;
        sl->status.message = "This build of Hydra has no ONNX Runtime (tools/fetch-onnxruntime.ps1)";
#else
        sl->status.state = State::noModel;
        if (modelFile ((Model) m).existsAsFile()) sl->status.message = "Downloaded, loads when a channel uses it";
#endif
        slots[(size_t) m] = std::move (sl);
    }
#if HYDRA_STEMS
    // Without a dedicated graphics card Stems is not offered (checked off the message thread, ~0.1 s).
    juce::Thread::launch ([]
    {
        if (dedicatedGpuName().isNotEmpty()) return;
        juce::MessageManager::callAsync ([]
        {
            if (auto& sv = serviceHolder(); sv != nullptr) sv->markNoGpu();
            logLine ("stems: no dedicated graphics card found, stem separation is not loaded");
        });
    });
#endif
}

void Service::markNoGpu()
{
    for (auto& sl : slots)
    {
        sl->loadFailures = 1;   // never loads (ensureModel)
        setState (*sl, State::unavailable, kNoGpuMessage);
    }
}

Service::~Service()
{
    for (auto& sl : slots) if (sl->worker != nullptr) sl->worker->stopThread (10000);
    for (auto& f : readyFlags) f.store (false);
}

juce::File Service::modelFile (Model m)
{
    const auto& info = modelInfo (m);
    const auto dir = appDataDir().getChildFile ("Models");
    const auto file = dir.getChildFile (info.file);
    if (info.unfolded != nullptr && ! file.existsAsFile())
        if (const auto old = dir.getChildFile (info.unfolded); old.existsAsFile()) return old;
    return file;
}

Service::Status Service::getStatus (Model m) const
{
    auto& sl = *slots[(size_t) m];
    Status s;
    {
        const std::lock_guard<std::mutex> lock (statusLock);
        s = sl.status;
    }
    s.inferenceMs = sl.avgMs.load();
    s.channels = 0;
    auto& self = const_cast<Service&> (*this);
    const std::lock_guard<std::mutex> lock (self.listLock);
    for (auto* inst : instances)
        if (const int mode = activeMode (*inst); mode >= 0 && modelFor (mode) == m) ++s.channels;
    return s;
}

double Service::loadOf (int mode, const Tuning& tuning) const
{
    const auto m = modelFor (mode);
    double ms = slots[(size_t) m]->avgMs.load();
    if (ms <= 0.0) ms = typicalMs (m);
    return ms / 1000.0 / timingFor (mode, tuning, referenceMs (m)).hop;
}

int Service::activeMode (Instance& inst) const
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (const auto b = inst.getBlocks(); b != inst.seenBlocks) { inst.seenBlocks = b; inst.seenAt = now; }
    return now - inst.seenAt < 500.0 ? inst.getMode() : -1;
}

double Service::loadOf (const Instance& inst) const
{
    int mode = -1;
    {
        auto& self = const_cast<Service&> (*this);
        const std::lock_guard<std::mutex> lock (self.listLock);
        if (std::find (instances.begin(), instances.end(), &inst) != instances.end()) mode = activeMode (const_cast<Instance&> (inst));
    }
    const double hop = inst.getHopSeconds();
    if (mode < 0 || hop <= 0.0 || ! isReady (modelFor (mode))) return 0.0;
    return slots[(size_t) modelFor (mode)]->avgMs.load() / 1000.0 / hop;
}

double Service::getLoad() const
{
    // Measured, not estimated: the time the models really ran in the last 3 s. The estimate from the average time per
    // window kept showing load while the effect was off (a stale slow run showed several hundred percent).
    constexpr double windowMs = 3000.0;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double from = now - windowMs;
    double busy = 0.0;
    for (auto& sl : slots)
    {
        const std::lock_guard<std::mutex> lock (sl->runsLock);
        // Only the part of each run inside the window, and the run in progress so far: the load moves smoothly
        // instead of jumping by a whole run when one ends.
        for (const auto& [end, ms] : sl->runs)
            if (ms > 0.0 && end > from) busy += juce::jmin (ms, end - from);
        if (sl->runStart >= 0.0) busy += now - juce::jmax (sl->runStart, from);
    }
    return busy / windowMs;
}

void Service::setState (Slot& sl, State st, const juce::String& message, float progress)
{
    const std::lock_guard<std::mutex> lock (statusLock);
    sl.status.state = st;
    sl.status.message = message;
    sl.status.progress = progress;
}

void Service::startWorkers()
{
#if HYDRA_STEMS
    for (auto& sl : slots)
    {
        if (sl->worker == nullptr) sl->worker = std::make_unique<Worker> (*this, *sl);
        if (! sl->worker->isThreadRunning()) sl->worker->startThread (juce::Thread::Priority::low);
        sl->worker->notify();
    }
#else
    for (auto& sl : slots)
        if (sl->testSeparator != nullptr)
        {
            if (sl->worker == nullptr) sl->worker = std::make_unique<Worker> (*this, *sl);
            if (! sl->worker->isThreadRunning()) sl->worker->startThread (juce::Thread::Priority::low);
        }
#endif
}

void Service::startDownload (Model m)
{
    auto& sl = *slots[(size_t) m];
    sl.cancel.store (false);
    sl.wantDownload.store (true);
    startWorkers();
}

void Service::cancelDownload (Model m) { slots[(size_t) m]->cancel.store (true); }

void Service::removeModel (Model m)
{
    auto& sl = *slots[(size_t) m];
    sl.wantUnload.store (true);
    if (sl.worker != nullptr && sl.worker->isThreadRunning())
    {
        sl.worker->notify();
        for (int i = 0; i < 200 && sl.wantUnload.load(); ++i) juce::Thread::sleep (10);
    }
    const auto& info = modelInfo (m);
    const auto dir = modelFile (m).getParentDirectory();
    for (auto* name : { info.file, info.unfolded })
        if (name != nullptr)
            for (auto* ext : { "", ".part", ".tmp" }) dir.getChildFile (juce::String (name) + ext).deleteFile();
    setState (sl, State::noModel);
}

void Service::add (Instance& inst)
{
    {
        const std::lock_guard<std::mutex> lock (listLock);
        if (std::find (instances.begin(), instances.end(), &inst) == instances.end()) instances.push_back (&inst);
    }
    for (auto& sl : slots) sl->loadFailures = 0;   // a new channel tries a model that failed to load once more
    startWorkers();
}

void Service::remove (Instance& inst)
{
    {
        const std::lock_guard<std::mutex> lock (listLock);
        instances.erase (std::remove (instances.begin(), instances.end(), &inst), instances.end());
    }
    const std::lock_guard<std::mutex> wait (inst.workLock);   // a job of it that runs right now ends first
}

bool Service::inUse (Model m)
{
    const std::lock_guard<std::mutex> lock (listLock);
    for (auto* inst : instances)
        if (const int mode = activeMode (*inst); mode >= 0 && modelFor (mode) == m) return true;
    return false;
}

void Service::setSeparatorForTests (Model m, std::unique_ptr<Separator> s)
{
    auto& sl = *slots[(size_t) m];
    sl.wantUnload.store (true);
    if (sl.worker != nullptr && sl.worker->isThreadRunning())
    {
        sl.worker->notify();
        for (int i = 0; i < 500 && sl.wantUnload.load(); ++i) juce::Thread::sleep (10);
    }
    sl.testSeparator = std::move (s);
    sl.loadFailures = 0;
    if (sl.worker != nullptr) sl.worker->notify();
}

void Service::setModelFileForTests (Model m, const juce::File& f) { slots[(size_t) m]->testModel = f; slots[(size_t) m]->loadFailures = 0; }

//==============================================================================
namespace
{
/** Folds a model's unfolded file into its folded one (ModelInfo::unfolded) and deletes the unfolded file. On failure
    the unfolded file stays and is used (it works, only loads slower). Worker thread. */
bool foldUnfolded (Model m)
{
    const auto& info = modelInfo (m);
    const auto dir = appDataDir().getChildFile ("Models");
    const auto from = dir.getChildFile (info.unfolded), to = dir.getChildFile (info.file);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    const bool ok = foldModel (from, to, error);
    if (ok) from.deleteFile();
    const juce::String name = info.name;
    const double s = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    juce::MessageManager::callAsync ([=]
    {
        logLine (ok ? "stems: " + name + " prepared for faster loading (" + juce::String (s, 1) + " s)"
                    : "stems: " + name + " could not be prepared for faster loading (" + error + "), loading it as it is");
    });
    return ok;
}
} // namespace

void Service::ensureModel (Slot& sl)
{
    auto& ready = readyFlags[(size_t) sl.id];
    if (ready.load() || sl.loadFailures > 0 || ! inUse (sl.id)) return;
#if HYDRA_STEMS
    if (sl.testSeparator == nullptr && sl.testModel == juce::File() && dedicatedGpuName().isEmpty())
    {
        sl.loadFailures = 1;
        setState (sl, State::unavailable, kNoGpuMessage);
        return;
    }
#endif
    if (sl.testSeparator != nullptr)
    {
        sl.avgMs.store (0.0);
        ready.store (true, std::memory_order_release);
        setState (sl, State::ready, "Test separator");
        return;
    }
#if HYDRA_STEMS
    auto file = sl.testModel != juce::File() ? sl.testModel : modelFile (sl.id);
    if (! file.existsAsFile())
    {
        if (const auto st = getStatus (sl.id).state; st != State::noModel && st != State::failed) setState (sl, State::noModel);
        return;
    }
    // Downloaded by an older Hydra: fold it once (a few seconds), every later load is faster.
    if (const auto* unfolded = modelInfo (sl.id).unfolded;
        sl.testModel == juce::File() && unfolded != nullptr && file.getFileName() == unfolded && ! sl.foldFailed)
    {
        setState (sl, State::preparing, "Preparing the model for faster loading (once)", 0.5f);
        if (foldUnfolded (sl.id)) file = modelFile (sl.id);
        else sl.foldFailed = true;
    }
    setState (sl, State::loading, "Loading the model onto the GPU");
    std::unique_ptr<Separator> sep;
    juce::String error;
    bool opened = false;
    if (sl.id == Model::roformer) { auto s = std::make_unique<RoformerSeparator>(); opened = s->open (file); error = s->lastError; sep = std::move (s); }
    else                          { auto s = std::make_unique<DemucsSeparator>();   opened = s->open (file); error = s->lastError; sep = std::move (s); }
    const juce::String name = modelInfo (sl.id).name;
    if (! opened)
    {
        ++sl.loadFailures;
        setState (sl, State::failed, error);
        juce::MessageManager::callAsync ([error, name] { logLine ("stems: " + name + " could not be loaded: " + error); });
        return;
    }
    // Warm-up: the first run compiles the GPU kernels (seconds); it also gives the first timing.
    const int n = sep->samples();
    std::vector<float> silence ((size_t) n * 2, 0.0f), outs ((size_t) n * 8);
    const float* in[2] = { silence.data(), silence.data() + n };
    float* out[8];
    for (int c = 0; c < 8; ++c) out[c] = outs.data() + (size_t) c * (size_t) n;
    for (int i = 0; i < 2; ++i)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        if (! sep->separate (in, out))
        {
            ++sl.loadFailures;
            setState (sl, State::failed, sep->lastError);
            return;
        }
        sl.avgMs.store (juce::Time::getMillisecondCounterHiRes() - t0);
    }
    sl.separator = std::move (sep);
    refMs[(size_t) sl.id].store (sl.avgMs.load());   // manual GPU settings derive their timing from this
    {
        const std::lock_guard<std::mutex> lock (statusLock);
        sl.status.device = gpuName();
    }
    ready.store (true, std::memory_order_release);
    setState (sl, State::ready);
    const auto ms = sl.avgMs.load();
    juce::MessageManager::callAsync ([ms, name] { logLine ("stems: " + name + " loaded, " + juce::String (juce::roundToInt (ms)) + " ms per window"); });
#endif
}

void Service::doDownload (Slot& sl)
{
#if HYDRA_STEMS
    if (sl.testModel == juce::File() && dedicatedGpuName().isEmpty())   // the model could never run here
    {
        setState (sl, State::unavailable, kNoGpuMessage);
        return;
    }
#endif
    const auto& info = modelInfo (sl.id);
    // The patched model; folded models are folded from it afterwards (foldUnfolded).
    const auto target = appDataDir().getChildFile ("Models").getChildFile (info.unfolded != nullptr ? info.unfolded : info.file);
    target.getParentDirectory().createDirectory();
    const auto part = target.getSiblingFile (juce::String (info.file) + ".part");
    setState (sl, State::downloading, "Connecting", 0.0f);
    juce::String error;
    {
        auto stream = juce::URL (info.url).createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress).withConnectionTimeoutMs (20000).withNumRedirectsToFollow (8));
        if (stream == nullptr) error = "No connection to huggingface.co";
        else
        {
            part.deleteFile();
            juce::FileOutputStream out (part);
            if (! out.openedOk()) error = "Cannot write " + part.getFullPathName();
            const int64_t total = stream->getTotalLength() > 0 ? stream->getTotalLength() : info.bytes;
            std::vector<char> chunk (1 << 20);
            int64_t done = 0;
            while (error.isEmpty() && ! stream->isExhausted())
            {
                if (sl.cancel.load() || sl.worker->threadShouldExit()) { error = "Cancelled"; break; }
                const int n = stream->read (chunk.data(), (int) chunk.size());
                if (n < 0) { error = "Download interrupted"; break; }
                if (n == 0) break;
                if (! out.write (chunk.data(), (size_t) n)) { error = "Disk full?"; break; }
                done += n;
                setState (sl, State::downloading, juce::String (done / 1000000) + " of " + juce::String (total / 1000000) + " MB",
                          (float) juce::jlimit (0.0, 1.0, (double) done / (double) total));
            }
            out.flush();
        }
    }
    if (error.isEmpty())
    {
        setState (sl, State::preparing, "Checking the file", 0.0f);
        juce::String hex;
        if (! sha256 (part, hex, sl.cancel)) error = sl.cancel.load() ? "Cancelled" : "Cannot read the download";
        else if (hex != info.sha256) error = "The downloaded file is not the expected model (checksum differs)";
    }
    if (error.isEmpty())
    {
        setState (sl, State::preparing, "Preparing the model for DirectML", 0.5f);
        juce::MemoryBlock model, fixed;
        if (! part.loadFileAsData (model)) error = "Cannot read the download";
        else if (const int n = patchForDirectML (model, fixed); n <= 0) error = "Unexpected model format";
        else
        {
            model.reset();
            const auto tmp = target.getSiblingFile (juce::String (info.file) + ".tmp");
            if (! tmp.replaceWithData (fixed.getData(), fixed.getSize()) || ! tmp.moveFileTo (target)) error = "Cannot write the model";
        }
    }
    if (error.isEmpty() && info.unfolded != nullptr)
    {
        setState (sl, State::preparing, "Preparing the model for faster loading", 0.8f);
        appDataDir().getChildFile ("Models").getChildFile (info.file).deleteFile();   // an older folded file
        sl.foldFailed = ! foldUnfolded (sl.id);   // failed: the unfolded file is used
    }
    part.deleteFile();
    const juce::String name = info.name;
    if (error.isNotEmpty())
    {
        setState (sl, State::failed, "Download: " + error);
        juce::MessageManager::callAsync ([error, name] { logLine ("stems: download of " + name + " failed: " + error); });
        return;
    }
    sl.loadFailures = 0;
    setState (sl, State::noModel, "Downloaded, loads when a channel uses it");
    juce::MessageManager::callAsync ([name] { logLine ("stems: " + name + " downloaded"); });
}

//==============================================================================
void Service::run (Slot& sl)
{
    auto& ready = readyFlags[(size_t) sl.id];
    while (! sl.worker->threadShouldExit())
    {
        if (sl.wantUnload.load())
        {
            ready.store (false, std::memory_order_release);
            sl.separator.reset();
            sl.wantUnload.store (false);
        }
        if (sl.wantDownload.exchange (false)) doDownload (sl);
        ensureModel (sl);

        // Free the GPU a little after the last channel stopped using this model (effect off, bypassed, removed, other
        // mode, no audio).
        const bool used = inUse (sl.id);
        if (ready.load() && ! used)
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (sl.idleSince < 0.0) sl.idleSince = now;
            else if (now - sl.idleSince > kUnloadSeconds * 1000.0)
            {
                ready.store (false, std::memory_order_release);
                sl.separator.reset();
                sl.idleSince = -1.0;
                setState (sl, State::noModel, "Unloaded (not in use)");
                const juce::String name = modelInfo (sl.id).name;
                juce::MessageManager::callAsync ([name] { logLine ("stems: " + name + " unloaded (not in use)"); });
            }
        }
        else sl.idleSince = -1.0;

        // The most urgent job of the channels using this model: smallest slack until its part is played.
        Instance* best = nullptr;
        int64_t bestJob = 0, bestSlack = std::numeric_limits<int64_t>::max();
        uint32_t bestEpoch = 0;
        int bestMode = 0;
        if (ready.load())
        {
            std::vector<Instance*> list;
            {
                const std::lock_guard<std::mutex> lock (listLock);
                list = instances;
            }
            for (auto* inst : list)
            {
                std::unique_lock<std::mutex> lock (inst->workLock, std::try_to_lock);
                if (! lock.owns_lock() || inst->inBuf.empty()) continue;
                // Sequence lock: the epoch, its timing and the written count belong together only if the epoch did
                // not change meanwhile (mode 0 = being changed).
                const uint32_t info = inst->epochInfo.load (std::memory_order_acquire);
                const Frames f = framesOf (*inst);
                const int64_t w = inst->writtenShared.load (std::memory_order_acquire);
                if (inst->epochInfo.load (std::memory_order_acquire) != info) continue;
                const uint32_t e = info >> 8;
                const int mode = (int) (info & 0xff) - 1;
                if (mode < 0 || modelFor (mode) != sl.id || f.hop <= 0) continue;
                if (e != inst->workEpoch) { inst->workEpoch = e; inst->nextJob = 0; inst->lastDone = -2; }
                const double fs = inst->sampleRate;
                const int hw = inst->hwIn;
                const int64_t avail = w - hw - f.fade / 2 - f.lookAhead;   // = (j + 1) * hop for the newest window
                if (avail < f.hop) continue;
                const int64_t newest = avail / f.hop - 1;
                if (newest < inst->nextJob) continue;
                int64_t job = inst->nextJob;
                const int64_t lat = f.latency (hw);
                const int64_t compute = (int64_t) (sl.avgMs.load() * 0.001 * fs * 1.2);
                auto slackOf = [&] (int64_t j) { return j * f.hop - f.fade / 2 + lat - w - compute; };
                if (slackOf (job) < 0) job = newest;   // too late for the next one: skip to the newest window
                if (const int64_t slack = slackOf (job); slack < bestSlack)
                {
                    best = inst; bestJob = job; bestSlack = slack; bestEpoch = e; bestMode = mode;
                }
            }
        }
        if (best == nullptr)
        {
            // Polls often only while a channel separates with this model (jobs come every hop); otherwise rarely.
            sl.worker->wait (ready.load() && used ? 10 : 250);
            continue;
        }
        const std::lock_guard<std::mutex> lock (best->workLock);
        if (best->workEpoch == bestEpoch && ! best->inBuf.empty()) runJob (sl, *best, bestJob, bestEpoch, bestMode);
    }
    ready.store (false);
    sl.separator.reset();
}

bool Service::runJob (Slot& sl, Instance& inst, int64_t j, uint32_t e, int mode)
{
    Separator* sep = sl.testSeparator != nullptr ? sl.testSeparator.get() : sl.separator.get();
    if (sep == nullptr) return false;
    const int window = sep->samples();
    const double fs = inst.sampleRate;
    const double step = fs / Separator::kRate;   // engine samples per model sample
    const bool sameRate = fs == (double) Separator::kRate;
    if (! sameRate && sl.ratesFs != fs)
    {
        sl.ratesFs = fs;
        sl.toModel = std::make_unique<Resampler> (step);
        sl.fromModel = std::make_unique<Resampler> (1.0 / step);
    }
    for (auto& w : sl.win) w.resize ((size_t) window);
    for (auto& s : sl.stemOut) for (auto& c : s) c.resize ((size_t) window);
    juce::ignoreUnused (mode);
    const Frames f = framesOf (inst);   // if the epoch changes meanwhile, the result is not published (step 4)
    if (f.hop <= 0) return false;
    const int hw = sameRate ? 0 : sl.toModel->halfWidth();
    const int64_t end = (j + 1) * f.hop + f.fade / 2 + f.lookAhead;   // window end (stream position)
    const double begin = (double) end - window * step;

    // 1. The window at 44.1 kHz (before the stream started: silence).
    const int64_t g0 = (int64_t) std::floor (begin) - hw - 1, g1 = end + hw + 1;
    sl.gather.resize ((size_t) (g1 - g0));
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = inst.in (ch);
        for (int64_t p = g0; p < g1; ++p) sl.gather[(size_t) (p - g0)] = p >= 0 ? src[(size_t) (p & inst.inMask)] : 0.0f;
        if (sameRate) std::memcpy (sl.win[ch].data(), sl.gather.data() + ((int64_t) begin - g0), (size_t) window * sizeof (float));
        else          sl.toModel->process (sl.gather.data(), (int) sl.gather.size(), begin - (double) g0, sl.win[ch].data(), window);
    }

    // 2. The model. A silent window (below -100 dBFS, e.g. playback paused) gives silent stems: no GPU work.
    const float* in[2] = { sl.win[0].data(), sl.win[1].data() };
    float* out[kModelStems * 2];
    for (int s = 0; s < kModelStems; ++s) for (int ch = 0; ch < 2; ++ch) out[s * 2 + ch] = sl.stemOut[s][ch].data();
    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto mm = juce::FloatVectorOperations::findMinAndMax (in[ch], window);
        peak = juce::jmax (peak, std::abs (mm.getStart()), std::abs (mm.getEnd()));
    }
    if (peak < 1.0e-5f)
    {
        for (auto* o : out) std::fill (o, o + window, 0.0f);
    }
    else
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        {
            const std::lock_guard<std::mutex> lock (sl.runsLock);
            sl.runStart = t0;
        }
        const bool ok = sep->separate (in, out);
        {
            const std::lock_guard<std::mutex> lock (sl.runsLock);
            sl.runStart = -1.0;
        }
        if (! ok)
        {
            setState (sl, State::failed, "Separation failed: " + sep->lastError);
            readyFlags[(size_t) sl.id].store (false, std::memory_order_release);
            sl.separator.reset();
            ++sl.loadFailures;
            return false;
        }
        const double t1 = juce::Time::getMillisecondCounterHiRes(), ms = t1 - t0;
        const double avg = sl.avgMs.load();
        sl.avgMs.store (avg <= 0.0 ? ms : avg + 0.2 * (ms - avg));
        const std::lock_guard<std::mutex> lock (sl.runsLock);
        sl.runs[(size_t) sl.runPos] = { t1, ms };
        sl.runPos = (sl.runPos + 1) % Slot::kRuns;
    }

    // 3. The part that is played, back at the engine rate, crossfaded with the previous window's part.
    const int64_t p0 = j * f.hop - f.fade / 2, p1 = (j + 1) * f.hop + f.fade / 2, len = p1 - p0;
    const bool joins = inst.lastDone == j - 1;
    sl.back.resize ((size_t) len);
    for (int s = 0; s < kModelStems; ++s)
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* stemData = out[s * 2 + ch];
            if (sameRate)
                for (int64_t q = 0; q < len; ++q)
                {
                    const int64_t m = p0 + q - (int64_t) begin;
                    sl.back[(size_t) q] = m >= 0 && m < window ? stemData[m] : 0.0f;
                }
            else
                sl.fromModel->process (stemData, window, ((double) p0 - begin) / step, sl.back.data(), (int) len);
            float* dst = inst.stem (s * 2 + ch);
            for (int64_t q = 0; q < len; ++q)
            {
                const auto at = (size_t) ((p0 + q) & inst.stemMask);
                const float v = sl.back[(size_t) q];
                if (q < f.fade)
                {
                    if (joins)
                    {
                        const float x = std::sin (juce::MathConstants<float>::halfPi * ((float) q + 0.5f) / (float) f.fade);
                        dst[at] += x * x * v;   // the previous part fades out here with cos^2
                    }
                    else dst[at] = v;
                }
                else if (q >= len - f.fade)
                {
                    const float x = std::cos (juce::MathConstants<float>::halfPi * ((float) (q - (len - f.fade)) + 0.5f) / (float) f.fade);
                    dst[at] = x * x * v;
                }
                else dst[at] = v;
            }
        }

    // 4. Publish (unless the stream started over meanwhile).
    if ((inst.epochInfo.load (std::memory_order_acquire) >> 8) != e) return false;
    if (! joins || inst.readyEpoch.load (std::memory_order_relaxed) != e) inst.validFrom.store (p0, std::memory_order_relaxed);
    inst.ready.store (p1 - f.fade, std::memory_order_relaxed);
    inst.readyEpoch.store (e, std::memory_order_release);
    inst.lastDone = j;
    inst.nextJob = j + 1;
    return true;
}
} // namespace hydra::stems
