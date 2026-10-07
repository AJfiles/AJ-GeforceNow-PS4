#include "color_simd.hpp"
#include "../stream_startup_diagnostics.hpp"

#include <emmintrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace opennow::color {

// Monotonic microsecond clock local to this file. The renderer has its own now_us(), but
// this translation unit must also build for the host tests, which do not link the PS4
// time functions.
inline uint64_t scale_now_us() {
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
            .count());
}

// Row-parallel worker pool used by the scaling converters.
//
// The scalers write every OUTPUT pixel of the 1920x1080 VideoOut buffer and do a
// scalar YUV->BGRA conversion per pixel. Measured on console: 40.8 ms per frame
// for a 720p stream and 40.8 ms for 540p, i.e. cost scales with output pixels and
// capped presentation at ~24 FPS. Splitting rows across the console's cores is the
// cheapest large win available without touching the decode path.
//
// Threads are created once and parked on a condition variable: spawning per frame
// would cost more than it saves. If thread creation fails the pool degrades to
// thread_count()==1 and the work simply runs on the calling thread.
class RowWorkerPool {
public:
    using RowFn = void (*)(int y_begin, int y_end);

    static RowWorkerPool& instance() {
        static RowWorkerPool pool;
        return pool;
    }

    // Total participants: the calling thread plus every worker.
    int thread_count() const { return static_cast<int>(workers_.size()) + 1; }

    // Runs fn over [0,rows) split into contiguous chunks and returns once every
    // chunk has finished. Never throws: on any synchronisation problem the caller
    // still completes the work single-threaded.
    //
    // Chunk assignment, and why the previous version was wrong:
    //   participants = workers + 1 (the caller)
    //   chunk 0              -> caller
    //   chunks 1..workers    -> worker (index) handles chunk (index)
    //   chunks               -> exactly `participants`, so every row is covered
    //                           once and pending counts only what workers own.
    //
    // The previous version derived `chunks = workers + 1` but had the caller take
    // chunk (chunks - 1) while worker index `i` ALSO took chunk `i` without an upper
    // bound check. With two workers that meant chunk 2 was executed twice: the caller
    // and worker 2 both processed rows [480,719). Effectively a 1.5x duplicate of the
    // whole frame, which is a large part of why the scaled path measured ~14 ms while
    // the 1:1 path measured 1.4-2.6 ms for the same output size.
    void run(RowFn fn, int rows) {
        const int workers = static_cast<int>(workers_.size());
        if (workers <= 0 || rows <= 1) {
            fn(0, rows);
            return;
        }
        const int chunks = std::min(workers + 1, rows);
        const int worker_chunks = chunks - 1; // caller owns exactly one chunk
        pending_.store(worker_chunks, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn_ = fn;
            rows_ = rows;
            chunks_ = worker_chunks; // workers must never exceed this
            ++generation_;
        }
        cv_.notify_all();
        // Bound the spin: if a worker fails to report, finish the caller's chunk and
        // return rather than hanging the render thread forever.
        const int chunk_rows = (rows + chunks - 1) / chunks;
        fn(0, std::min(rows, chunk_rows)); // caller takes chunk 0
        int spins = 0;
        while (pending_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
            if (++spins > 200000) break; // ~seconds; never hang the console
        }
        // Prevent the next dispatch from reusing a worker that has not yet
        // observed the new generation.
        std::lock_guard<std::mutex> lock(mutex_);
        fn_ = nullptr;
    }

private:
    RowWorkerPool() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 2;
        // PARALLELISM. `participants` counts the calling thread too, and the loop below
        // creates one worker per participant beyond the first.
        //
        // The previous version computed `wanted` as min(hw-1, 3) and then created
        // `wanted - 1` workers, so `wanted` was being treated as a worker count in one
        // place and a participant count in the other. On a 2-core host that yielded
        // wanted=1 and therefore ZERO workers, and thread_count() reported 1: the scaler
        // silently ran entirely on the calling thread. Verified with the host benchmark
        // (tests/scale_path_bench.cpp), which prints the pool's participant count.
        //
        // Leave headroom for the decode and network workers: PS4 exposes 6 usable cores,
        // so 3 scaler participants plus the decoder fit without oversubscribing.
        //
        // A floor of 2 participants guarantees at least one worker even on a 2-core
        // machine, where hw-1 would otherwise be 1 and the scaler would run entirely in
        // series. Verified by printing the participant count from the host benchmark.
        // PARALELISMO: subido de 3 a 6 participantes.
        //
        // MEDIDO en consola (3.23, sesion con escalado 540p->720p):
        //     VIDEOOUT_PRESENT_STAGES convert_us=647182 para 37 frames
        //     -> 17.491 us por frame SOLO en conversion de color y escalado
        //     VIDEOOUT_SCALE_BILINEAR_US avg=26591 max=36724
        // El presupuesto a 60 FPS es 16.666 us: con 3 participantes la conversion ya lo superaba por
        // si sola, y esa es la causa de que toda la imagen se degradara al bajar el servidor a 540p.
        //
        // El limite anterior era 3 con el comentario "PS4 expone 6 nucleos", pero la PS4 (Jaguar)
        // tiene 8. Con el decodificador en 5 hilos, 6 participantes en el escalado reparten el
        // trabajo y dejan margen al hilo de red y al de presentacion.
        // =============================================================================================
        // PARTICIPANTES DEL ESCALADOR: 6 -> 4 (v4.19). ES LO UNICO QUE QUEDA POR ATACAR DEL COSTE.
        // =============================================================================================
        // LAS MEDICIONES QUE LO MOTIVAN, de los logs de consola (v4.13 y v4.15):
        //
        //     VIDEOOUT_SCALE_TIMING  dispatch_avg_us=35.462  threads=6  dst=1920x1080
        //     DECODE_PERF            hilos=5
        //     UI_LOOP_BUDGET page=5  iter_ms=24-31  frames=32-41
        //
        // **6 participantes en el escalado + 5 hilos del decodificador = 11 hilos.** Y la PS4 (Jaguar)
        // tiene **8 nucleos**. Es decir: **se esta sobrecargando el procesador**, y cuando eso pasa los
        // hilos se quitan la CPU unos a otros y **todos van mas lentos que si fueran menos**.
        //
        // Y hay una referencia que lo confirma: **el mismo escalado medido en el PC cuesta 12,7 ms**
        // (`PS4-V3.65`), frente a los **35,5 ms** de la consola. Un factor de 2,8× **no se explica solo
        // por la diferencia de reloj**; la contencion es la explicacion que encaja.
        //
        // EL PROPIO PROYECTO YA LO HABIA DICHO, y el comentario seguia ahi mientras el valor era 6:
        //
        //     "Leave headroom for the decode and network workers: PS4 exposes 6 usable cores,
        //      so 3 scaler participants plus the decoder fit without oversubscribing."
        //
        // **3 participantes + el decodificador caben sin sobrecargar.** Se subio a 6 cambiando el
        // razonamiento a "la PS4 tiene 8 nucleos", pero **hay 3 nucleos mas de trabajo ademas del
        // escalado y del decodificador**: red, entrada, audio y el propio bucle de presentacion. Con 11
        // hilos en 8 nucleos, esa cuenta no sale.
        //
        // SE BAJA A 4 (3 hilos + el llamante), que deja 4 nucleos para el resto. **Es una hipotesis con
        // base en las medidas, no una certeza**: si `dispatch_avg_us` no baja, entonces el coste NO es
        // contencion sino memoria o calculo, y el siguiente paso seria el escalador en si.
        const unsigned capped = std::min(hw > 1 ? hw - 1 : 1u, 4u);
        const unsigned participants = capped < 2u ? 2u : capped;
        const unsigned wanted_workers = participants - 1;
        try {
            for (unsigned i = 0; i < wanted_workers; ++i) {
                workers_.emplace_back([this, i] { worker_loop(static_cast<int>(i)); });
            }
        } catch (...) {
            // Partial pool is still usable; keep whatever started.
        }
    }

    ~RowWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
            ++generation_;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    void worker_loop(int index) {
        uint64_t seen = 0;
        for (;;) {
            RowFn fn = nullptr;
            int rows = 0, chunks = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return shutdown_ || generation_ != seen; });
                if (shutdown_) return;
                seen = generation_;
                fn = fn_;
                rows = rows_;
                chunks = chunks_;
            }
            // Worker `index` owns chunk `index` in [1, chunks]. Chunk 0 belongs to the
            // caller. Bounding this is essential: without the check, worker indices
            // beyond the chunk count used to re-run the caller's last chunk (the
            // duplicated-work bug), and workers with no work still decremented
            // pending_, which could drive it negative and let run() return early.
            const int chunk_index = index + 1;
            if (fn && chunks > 0 && chunk_index <= chunks) {
                const int total_chunks = chunks + 1; // + the caller's chunk 0
                const int chunk_rows = (rows + total_chunks - 1) / total_chunks;
                const int begin = chunk_index * chunk_rows;
                const int end = std::min(rows, begin + chunk_rows);
                if (begin < end) fn(begin, end);
                pending_.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    RowFn fn_ = nullptr;
    int rows_ = 0;
    int chunks_ = 0;
    uint64_t generation_ = 0;
    bool shutdown_ = false;
    std::atomic<int> pending_{0};
};

namespace {

inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// BT.709 per-pixel calculation (Q7 scale, 128 = 1.0)
// Limited range: Y in [16, 235], U,V in [16, 240]
// Y_val = (Y - 16) * 149
// R = (Y_val + 230 * (V - 128)) >> 7
// G = (Y_val - 27 * (U - 128) - 68 * (V - 128)) >> 7
// B = (Y_val + 270 * (U - 128)) >> 7
// Full range: Y in [0, 255], U,V in [0, 255]
// Y_val = Y * 128
// R = (Y_val + 202 * (V - 128)) >> 7
// G = (Y_val - 24 * (U - 128) - 60 * (V - 128)) >> 7
// B = (Y_val + 238 * (U - 128)) >> 7
inline void yuv_to_bgra_px(uint8_t* dp, int y_raw, int uu, int vv, bool full_range = false) {
    int y_scaled;
    int r, g, b;
    if (full_range) {
        y_scaled = y_raw * 128;
        r = (y_scaled + 202 * vv) >> 7;
        g = (y_scaled - 24 * uu - 60 * vv) >> 7;
        b = (y_scaled + 238 * uu) >> 7;
    } else {
        int y_adj = y_raw - 16;
        if (y_adj < 0) y_adj = 0;
        y_scaled = y_adj * 149;
        r = (y_scaled + 230 * vv) >> 7;
        g = (y_scaled - 27 * uu - 68 * vv) >> 7;
        b = (y_scaled + 270 * uu) >> 7;
    }

    dp[0] = clamp_u8(b);
    dp[1] = clamp_u8(g);
    dp[2] = clamp_u8(r);
    dp[3] = 0xFF;
}

inline __attribute__((always_inline)) void
bgra_store8(uint8_t* dp, __m128i y_val, __m128i rv, __m128i guv, __m128i bu,
            __m128i a255, const int store_mode) {
    // Both y_val and chroma terms (rv, guv, bu) are pre-shifted by >> 7,
    // so they fit comfortably in signed 16-bit range without overflow!
    __m128i r16 = _mm_add_epi16(y_val, rv);
    __m128i g16 = _mm_sub_epi16(y_val, guv);
    __m128i b16 = _mm_add_epi16(y_val, bu);

    // =================================================================================================
    // ORDEN DE CANALES EN MEMORIA: **`[B][G][R][A]`**. NO CAMBIARLO SIN LEER ESTO.
    // =================================================================================================
    // El valor de 32 bits que se empaqueta aqui es **0xAARRGGBB**, que en little-endian deja los bytes
    // en el orden **`[B][G][R][A]`**. Ese es el contrato de esta funcion (`color_simd.hpp`:
    // *"dst_bgra: output buffer in BGRA format"*), y **lo rompi en la v4.14 intentando arreglar el
    // color desde aqui. Lo revierto y queda escrito por que NO se puede hacer en este punto.**
    //
    // ESTA FUNCION TIENE DOS USUARIOS, y **los dos esperan `[B][G][R][A]`**:
    //
    //   1. **`PS4VideoOutRenderer.cpp`** (ruta directa) escribe en el framebuffer de VideoOut, que esta
    //      registrado como `A8B8G8R8_SRGB`. El emulador lo traduce a **`RGBA8`**
    //      (`shadPS4`, `vk_presenter.cpp:651`), es decir **lee `[R][G][B][A]`**... y aun asi el color
    //      sale BIEN. Eso significa que el nombre `A8B8G8R8` de OpenOrbis **se lee al reves**: el
    //      hardware espera `[B][G][R][A]` con el alfa en el byte alto, que es exactamente lo que esta
    //      funcion escribe.
    //   2. **`SDLVideoRenderer.cpp`** (ruta SDL) escribe en una textura `SDL_PIXELFORMAT_ARGB8888`,
    //      cuyo valor es `0xAARRGGBB` = **tambien `[B][G][R][A]`**. Coincide.
    //
    // Y la PRUEBA de que `[B][G][R][A]` es lo correcto: `tests/video_color_path_test.cpp` comprueba en
    // la consola del PC que *"U alta / V baja da AZUL (B > R)"* y *"U baja / V alta da ROJO (R > B)"*
    // leyendo los bytes en ese orden. **Con mi cambio ese test fallo** (`r=213 b=0` y `r=0 b=193`), que
    // es la senal de que estaba rompiendo el contrato, no arreglando nada.
    //
    // DONDE SI HAY QUE COMPENSAR EL ORDEN: en el LIENZO de la ruta SDL (ver `SDLVideoRenderer.cpp` y
    // `main.cpp`), y **solo ahi**, porque el driver de SDL-PS4 tiene su propia incoherencia de formato.
    // **El color del video ya estaba bien en la v4.11** — lo dijo el usuario — asi que el problema a
    // resolver era SOLO el coste del blit, no el color.
    __m128i br = _mm_packus_epi16(b16, r16);
    __m128i ga = _mm_packus_epi16(g16, a255);

    // Interleave channels to BGRA:
    // br: B0 B1 B2 B3 B4 B5 B6 B7 R0 R1 R2 R3 R4 R5 R6 R7
    // ga: G0 G1 G2 G3 G4 G5 G6 G7 A0 A1 A2 A3 A4 A5 A6 A7
    __m128i bg0 = _mm_unpacklo_epi8(br, ga); // B0 G0 B1 G1 B2 G2 B3 G3 B4 G4 B5 G5 B6 G6 B7 G7
    __m128i ra0 = _mm_unpackhi_epi8(br, ga); // R0 A0 R1 A1 R2 A2 R3 A3 R4 A4 R5 A5 R6 A6 R7 A7

    __m128i px0 = _mm_unpacklo_epi16(bg0, ra0); // 4 pixels: BGRA 0..3
    __m128i px1 = _mm_unpackhi_epi16(bg0, ra0); // 4 pixels: BGRA 4..7

    if (store_mode == STORE_STREAMING) {
        _mm_stream_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp)), px0);
        _mm_stream_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp + 16)), px1);
    } else if (store_mode == STORE_ALIGNED) {
        _mm_store_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp)), px0);
        _mm_store_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp + 16)), px1);
    } else {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp)), px0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<void*>(dp + 16)), px1);
    }
}

} // namespace

void ConvertYUV420PToBGRA_BT709_Scalar(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    bool full_range) {
    for (int y = 0; y < height; y++) {
        uint8_t* drow = dst_bgra + static_cast<size_t>(y) * dst_pitch;
        const uint8_t* yrow = y_plane + static_cast<size_t>(y) * y_pitch;
        const uint8_t* urow = u_plane + static_cast<size_t>(y / 2) * u_pitch;
        const uint8_t* vrow = v_plane + static_cast<size_t>(y / 2) * v_pitch;
        for (int x = 0; x < width; x++) {
            int uu = static_cast<int>(urow[x / 2]) - 128;
            int vv = static_cast<int>(vrow[x / 2]) - 128;
            yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, yrow[x], uu, vv, full_range);
        }
    }
}

void ConvertNV12ToBGRA_BT709_Scalar(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    bool full_range) {
    for (int y = 0; y < height; y++) {
        uint8_t* drow = dst_bgra + static_cast<size_t>(y) * dst_pitch;
        const uint8_t* yrow = y_plane + static_cast<size_t>(y) * y_pitch;
        const uint8_t* uvrow = uv_plane + static_cast<size_t>(y / 2) * uv_pitch;
        for (int x = 0; x < width; x++) {
            int uu = static_cast<int>(uvrow[(x / 2) * 2]) - 128;
            int vv = static_cast<int>(uvrow[(x / 2) * 2 + 1]) - 128;
            yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, yrow[x], uu, vv, full_range);
        }
    }
}

void ConvertYUV420PToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    int store_mode,
    bool full_range) {
    const __m128i zero   = _mm_setzero_si128();
    const __m128i c16    = _mm_set1_epi16(16);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c_y_mult = _mm_set1_epi16(full_range ? 128 : 149);
    const __m128i c_rv_mult = _mm_set1_epi16(full_range ? 202 : 230);
    const __m128i c_gu_mult = _mm_set1_epi16(full_range ? 24 : 27);
    const __m128i c_gv_mult = _mm_set1_epi16(full_range ? 60 : 68);
    const __m128i c_bu_mult = _mm_set1_epi16(full_range ? 238 : 270);
    const __m128i a255   = _mm_set1_epi16(255);

    int row = 0;
    for (; row + 1 < height; row += 2) {
        uint8_t* drow0 = dst_bgra + static_cast<size_t>(row) * dst_pitch;
        uint8_t* drow1 = drow0 + dst_pitch;
        const uint8_t* yrow0 = y_plane + static_cast<size_t>(row) * y_pitch;
        const uint8_t* yrow1 = yrow0 + y_pitch;
        const uint8_t* urow = u_plane + static_cast<size_t>(row / 2) * u_pitch;
        const uint8_t* vrow = v_plane + static_cast<size_t>(row / 2) * v_pitch;

        int x = 0;
        for (; x + 15 < width; x += 16) {
            _mm_prefetch(reinterpret_cast<const char*>(yrow1 + y_pitch + x), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(urow + u_pitch + (x / 2)), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(vrow + v_pitch + (x / 2)), _MM_HINT_T0);

            // Load 8 bytes of U and 8 bytes of V (which cover 16 pixels)
            __m128i u8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(static_cast<const void*>(urow + (x / 2))));
            __m128i v8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(static_cast<const void*>(vrow + (x / 2))));

            __m128i u16 = _mm_sub_epi16(_mm_unpacklo_epi8(u8, zero), c128);
            __m128i v16 = _mm_sub_epi16(_mm_unpacklo_epi8(v8, zero), c128);

            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c_rv_mult), 7);
            __m128i guv = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(u16, c_gu_mult), _mm_mullo_epi16(v16, c_gv_mult)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c_bu_mult), 7);

            // Duplicate chroma for adjacent horizontal pixel pairs
            __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
            __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
            __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
            __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
            __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
            __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

            // Row 0 Y samples
            __m128i y0_raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const void*>(yrow0 + x)));
            __m128i y0_adj_lo = _mm_unpacklo_epi8(y0_raw, zero);
            __m128i y0_adj_hi = _mm_unpackhi_epi8(y0_raw, zero);
            if (!full_range) {
                y0_adj_lo = _mm_max_epi16(_mm_sub_epi16(y0_adj_lo, c16), zero);
                y0_adj_hi = _mm_max_epi16(_mm_sub_epi16(y0_adj_hi, c16), zero);
            }
            __m128i y0_sc_lo = _mm_srai_epi16(_mm_mullo_epi16(y0_adj_lo, c_y_mult), 7);
            __m128i y0_sc_hi = _mm_srai_epi16(_mm_mullo_epi16(y0_adj_hi, c_y_mult), 7);

            // Row 1 Y samples
            __m128i y1_raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const void*>(yrow1 + x)));
            __m128i y1_adj_lo = _mm_unpacklo_epi8(y1_raw, zero);
            __m128i y1_adj_hi = _mm_unpackhi_epi8(y1_raw, zero);
            if (!full_range) {
                y1_adj_lo = _mm_max_epi16(_mm_sub_epi16(y1_adj_lo, c16), zero);
                y1_adj_hi = _mm_max_epi16(_mm_sub_epi16(y1_adj_hi, c16), zero);
            }
            __m128i y1_sc_lo = _mm_srai_epi16(_mm_mullo_epi16(y1_adj_lo, c_y_mult), 7);
            __m128i y1_sc_hi = _mm_srai_epi16(_mm_mullo_epi16(y1_adj_hi, c_y_mult), 7);

            bgra_store8(drow0 + static_cast<size_t>(x) * 4, y0_sc_lo, rv_lo, guv_lo, bu_lo, a255, store_mode);
            bgra_store8(drow0 + static_cast<size_t>(x) * 4 + 32, y0_sc_hi, rv_hi, guv_hi, bu_hi, a255, store_mode);
            bgra_store8(drow1 + static_cast<size_t>(x) * 4, y1_sc_lo, rv_lo, guv_lo, bu_lo, a255, store_mode);
            bgra_store8(drow1 + static_cast<size_t>(x) * 4 + 32, y1_sc_hi, rv_hi, guv_hi, bu_hi, a255, store_mode);
        }

        for (; x < width; x++) {
            int uu = static_cast<int>(urow[x / 2]) - 128;
            int vv = static_cast<int>(vrow[x / 2]) - 128;
            yuv_to_bgra_px(drow0 + static_cast<size_t>(x) * 4, yrow0[x], uu, vv, full_range);
            yuv_to_bgra_px(drow1 + static_cast<size_t>(x) * 4, yrow1[x], uu, vv, full_range);
        }
    }

    for (; row < height; row++) {
        uint8_t* drow = dst_bgra + static_cast<size_t>(row) * dst_pitch;
        const uint8_t* yrow = y_plane + static_cast<size_t>(row) * y_pitch;
        const uint8_t* urow = u_plane + static_cast<size_t>(row / 2) * u_pitch;
        const uint8_t* vrow = v_plane + static_cast<size_t>(row / 2) * v_pitch;
        for (int x = 0; x < width; x++) {
            int uu = static_cast<int>(urow[x / 2]) - 128;
            int vv = static_cast<int>(vrow[x / 2]) - 128;
            yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, yrow[x], uu, vv, full_range);
        }
    }
}

void ConvertNV12ToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int width, int height,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    int store_mode,
    bool full_range) {
    const __m128i zero   = _mm_setzero_si128();
    const __m128i m00ff  = _mm_set1_epi16(0x00FF);
    const __m128i c16    = _mm_set1_epi16(16);
    const __m128i c128   = _mm_set1_epi16(128);
    const __m128i c_y_mult = _mm_set1_epi16(full_range ? 128 : 149);
    const __m128i c_rv_mult = _mm_set1_epi16(full_range ? 202 : 230);
    const __m128i c_gu_mult = _mm_set1_epi16(full_range ? 24 : 27);
    const __m128i c_gv_mult = _mm_set1_epi16(full_range ? 60 : 68);
    const __m128i c_bu_mult = _mm_set1_epi16(full_range ? 238 : 270);
    const __m128i a255   = _mm_set1_epi16(255);

    int row = 0;
    for (; row + 1 < height; row += 2) {
        uint8_t* drow0 = dst_bgra + static_cast<size_t>(row) * dst_pitch;
        uint8_t* drow1 = drow0 + dst_pitch;
        const uint8_t* yrow0 = y_plane + static_cast<size_t>(row) * y_pitch;
        const uint8_t* yrow1 = yrow0 + y_pitch;
        const uint8_t* uvrow = uv_plane + static_cast<size_t>(row / 2) * uv_pitch;

        int x = 0;
        for (; x + 15 < width; x += 16) {
            _mm_prefetch(reinterpret_cast<const char*>(yrow1 + y_pitch + x), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(uvrow + uv_pitch + x), _MM_HINT_T0);

            // 16B interleaved UV = chroma for 16 px (8 pairs of U,V)
            __m128i uv8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const void*>(uvrow + x)));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);

            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c_rv_mult), 7);
            __m128i guv = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(u16, c_gu_mult), _mm_mullo_epi16(v16, c_gv_mult)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c_bu_mult), 7);

            __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
            __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
            __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
            __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
            __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
            __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

            __m128i y0_raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const void*>(yrow0 + x)));
            __m128i y0_adj_lo = _mm_unpacklo_epi8(y0_raw, zero);
            __m128i y0_adj_hi = _mm_unpackhi_epi8(y0_raw, zero);
            if (!full_range) {
                y0_adj_lo = _mm_max_epi16(_mm_sub_epi16(y0_adj_lo, c16), zero);
                y0_adj_hi = _mm_max_epi16(_mm_sub_epi16(y0_adj_hi, c16), zero);
            }
            __m128i y0_sc_lo = _mm_srai_epi16(_mm_mullo_epi16(y0_adj_lo, c_y_mult), 7);
            __m128i y0_sc_hi = _mm_srai_epi16(_mm_mullo_epi16(y0_adj_hi, c_y_mult), 7);

            __m128i y1_raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<const void*>(yrow1 + x)));
            __m128i y1_adj_lo = _mm_unpacklo_epi8(y1_raw, zero);
            __m128i y1_adj_hi = _mm_unpackhi_epi8(y1_raw, zero);
            if (!full_range) {
                y1_adj_lo = _mm_max_epi16(_mm_sub_epi16(y1_adj_lo, c16), zero);
                y1_adj_hi = _mm_max_epi16(_mm_sub_epi16(y1_adj_hi, c16), zero);
            }
            __m128i y1_sc_lo = _mm_srai_epi16(_mm_mullo_epi16(y1_adj_lo, c_y_mult), 7);
            __m128i y1_sc_hi = _mm_srai_epi16(_mm_mullo_epi16(y1_adj_hi, c_y_mult), 7);

            bgra_store8(drow0 + static_cast<size_t>(x) * 4, y0_sc_lo, rv_lo, guv_lo, bu_lo, a255, store_mode);
            bgra_store8(drow0 + static_cast<size_t>(x) * 4 + 32, y0_sc_hi, rv_hi, guv_hi, bu_hi, a255, store_mode);
            bgra_store8(drow1 + static_cast<size_t>(x) * 4, y1_sc_lo, rv_lo, guv_lo, bu_lo, a255, store_mode);
            bgra_store8(drow1 + static_cast<size_t>(x) * 4 + 32, y1_sc_hi, rv_hi, guv_hi, bu_hi, a255, store_mode);
        }

        for (; x < width; x++) {
            int uu = static_cast<int>(uvrow[(x / 2) * 2]) - 128;
            int vv = static_cast<int>(uvrow[(x / 2) * 2 + 1]) - 128;
            yuv_to_bgra_px(drow0 + static_cast<size_t>(x) * 4, yrow0[x], uu, vv, full_range);
            yuv_to_bgra_px(drow1 + static_cast<size_t>(x) * 4, yrow1[x], uu, vv, full_range);
        }
    }

    for (; row < height; row++) {
        uint8_t* drow = dst_bgra + static_cast<size_t>(row) * dst_pitch;
        const uint8_t* yrow = y_plane + static_cast<size_t>(row) * y_pitch;
        const uint8_t* uvrow = uv_plane + static_cast<size_t>(row / 2) * uv_pitch;
        for (int x = 0; x < width; x++) {
            int uu = static_cast<int>(uvrow[(x / 2) * 2]) - 128;
            int vv = static_cast<int>(uvrow[(x / 2) * 2 + 1]) - 128;
            yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, yrow[x], uu, vv, full_range);
        }
    }
}

// Shared per-frame state for the row-parallel scalers. Written once before
// dispatch and read-only while workers run.
struct ScaleJob {
    uint8_t* dst_bgra = nullptr;
    int dst_pitch = 0;
    int dst_w = 0;
    int src_w = 0;
    const uint8_t* y_plane = nullptr;
    const uint8_t* u_plane = nullptr;
    const uint8_t* v_plane = nullptr;
    const uint8_t* uv_plane = nullptr; // NV12 only
    int y_pitch = 0;
    int u_pitch = 0;
    int v_pitch = 0;
    int uv_pitch = 0;
    bool full_range = false;
    const int* sx_map = nullptr;
    const int* sux_map = nullptr;
    uint32_t y_ratio = 0;
    int src_h = 0;
    bool yuv420 = true;
    // Fila temporal para la mezcla VERTICAL. El mapeo de filas era vecino mas cercano
    // (map_source_row devuelve una sola fila), asi que a 1.33x cada fila de origen se
    // duplicaba de forma irregular: unas una vez y otras dos. Eso produce un parpadeo
    // visible en cada frame, que es la "actualizacion de imagen" que reporto el usuario.
    // Aqui se mezclan las dos filas de origen adyacentes segun v_weight antes de escalar
    // en horizontal, con lo que el coste por pixel no cambia (solo se lee una fila mas).
    // Peso de la mezcla vertical para ESTA tanda de filas (0..256). La mezcla se hace dentro de
    // la funcion de fila, sin buffer intermedio: g_scale_job es un UNICO global compartido por
    // todos los workers del pool, asi que un buffer aqui provocaba una condicion de carrera y
    // producia bandas horizontales (filas mezcladas de franjas distintas).
    bool y_vertical_blend = false;
    // Fuerza del realce de luma (0 = desactivado). Se calcula a partir del factor de escala.
    int sharpen_k = 0;
    // Peso de la mezcla vertical, 0..256. 0 = usar solo la fila de origen.
    int v_weight = 0;
    // Force the old scalar per-pixel path. Kept as an escape hatch: if the SSE2
    // scaled row converter ever produces wrong output on real hardware, this restores
    // the previous behaviour without a rebuild of the logic.
    bool scalar_fallback = false;
    // Store mode used by the SSE2 path (STORE_* constants from color_simd.hpp).
    int store_mode = STORE_STREAMING;
};

ScaleJob g_scale_job;

// Maps an output row to its source row once per dispatch so the row loop stays
// division-free.
int map_source_row(int y, const ScaleJob& job) {
    int sy = static_cast<int>((static_cast<uint32_t>(y) * job.y_ratio) >> 16);
    if (sy >= job.src_h) sy = job.src_h - 1;
    if (sy < 0) sy = 0;
    return sy;
}

// Igual que map_source_row, pero devuelve ademas el peso fraccionario (0..256) de la fila
// SIGUIENTE, que es lo que convierte el escalado vertical en bilineal de verdad.
//
// El calculo es el mismo producto en punto fijo que usa map_source_row, pero conservando los
// 16 bits bajos en vez de descartarlos: la parte entera es la fila y la fraccion es el peso.
int map_source_row_weighted(int y, const ScaleJob& job, int& weight) {
    const uint32_t fixed = static_cast<uint32_t>(y) * job.y_ratio;
    int sy = static_cast<int>(fixed >> 16);
    weight = static_cast<int>((fixed >> 8) & 0xFF);
    if (sy >= job.src_h - 1) { sy = job.src_h - 1; weight = 0; }
    if (sy < 0) { sy = 0; weight = 0; }
    return sy;
}

// SIMD scaled row conversion.
//
// WHY THIS EXISTS: the 1:1 path measured 1.4-2.6 ms for a 1280x720 output while the
// scaled path measured 13.5-15 ms for the SAME output size. The difference was not
// the scaling itself but that the scaled path used the scalar per-pixel
// yuv_to_bgra_px() (integer multiplies, branches, four byte stores) while the 1:1
// path used the SSE2 bgra_store8() pipeline. That 6x gap is what pushed the scaled
// case to ~90% of a 16.7 ms frame budget and left no margin, which is when tearing
// appears.
//
// This keeps the scalar GATHER (the source x indices come from a map, so they cannot
// be loaded as a contiguous vector) but moves the pixel arithmetic into the same SSE2
// pipeline the 1:1 path uses, in blocks of 8 output pixels.
//
// INT16 OVERFLOW, verified on the host (tests/scale_simd_dump.cpp):
//   Every intermediate in this conversion can exceed signed 16-bit (32767), and each
//   one produced a visible failure that the host test caught:
//     * Y term, limited range: y_adj reaches 239, 239 * 149 = 35611 -> wraps negative.
//       Symptom: whole pixel black (B=0 G=0 R=0) at high Y.
//     * B chroma term: 270 * 127 = 34290, and the SUM y_sc + bu reaches 278 + 267 = 545
//       with y_sc at its maximum, which also wraps negative. Symptom: B saturating to 0
//       or 255 wrongly at extreme chroma (u=252 v=254 -> SIMD B=0, scalar B=255).
//
//   Both the Y scale and the three chroma terms are therefore computed in scalar during
//   the gather, where full int width is available, and loaded as int16. Only the final
//   add/subtract/pack/saturate stage stays vectorised, which is where the work actually
//   is: the multiplies are at most 8 per row-chunk because chroma is horizontally
//   subsampled, while the packing covers dst_w pixels.
// ---------------------------------------------------------------------------
// MEZCLA VERTICAL DE LUMA
//
// PROBLEMA QUE RESUELVE: map_source_row() devuelve UNA sola fila de origen, es decir, el
// escalado vertical era vecino mas cercano. A 1.33x (540p -> 720p) eso reparte las filas asi:
//
//   fila destino : 0  1  2  3  4  5  6  7 ...
//   fila origen  : 0  0  1  2  3  3  4  5 ...   <- duplicados IRREGULARES
//
// Unas filas de origen aparecen una vez y otras dos, y como el contenido se mueve, el patron
// de duplicados cambia en cada frame. El resultado es un parpadeo o "temblor" continuo en los
// bordes horizontales y en los textos: la "actualizacion de imagen" que reporto el usuario.
//
// SOLUCION: mezclar las dos filas de origen adyacentes con el peso fraccionario antes de
// escalar en horizontal. El escalado horizontal (SSE2) no se toca, y el coste por pixel apenas
// cambia: se lee una fila mas y se hacen dos multiplicaciones y un desplazamiento.
//
// Solo se aplica a LUMA. La crominancia esta subsampleada 2x2 en YUV420, asi que su resolucion
// vertical ya es la mitad: interpolar ahi no aporta nada visible y costaria lo mismo.
// ---------------------------------------------------------------------------
// MEZCLA VERTICAL DE LUMA, POR PIXEL
//
// PROBLEMA QUE RESUELVE: map_source_row() devuelve UNA sola fila de origen, es decir, el escalado
// vertical era vecino mas cercano. A 1.33x (540p -> 720p) eso reparte las filas asi:
//
//   fila destino : 0  1  2  3  4  5  6  7 ...
//   fila origen  : 0  0  1  2  3  3  4  5 ...   <- duplicados IRREGULARES
//
// Unas filas de origen aparecen una vez y otras dos, y como el contenido se mueve, el patron de
// duplicados cambia en cada frame: eso es un parpadeo continuo en los bordes horizontales y los
// textos.
//
// POR QUE NO HAY BUFFER INTERMEDIO: la primera version de este arreglo mezclaba las dos filas en un
// buffer dentro de ScaleJob. Eso produjo BANDAS HORIZONTALES en consola, porque g_scale_job es un
// UNICO global compartido: el pool lanza varios workers que escalan franjas distintas a la vez, y
// todos escribian en el mismo buffer. Mezclar aqui, pixel a pixel, no necesita memoria y por tanto
// no puede tener carreras.
//
// El coste es una lectura extra y dos operaciones por pixel, solo cuando hay escalado vertical.
inline int blend_luma_px(int a, int b, int weight) {
    if (weight <= 0) return a;
    if (weight >= 256) return b;
    return (a * (256 - weight) + b * weight + 128) >> 8;
}
// ---------------------------------------------------------------------------
// REALCE DE NITIDEZ ADAPTATIVO â€” ALGORITMO RCAS DE AMD
//
// ORIGEN DEL ALGORITMO
// --------------------
// AMD FidelityFX Super Resolution 1.0, pase RCAS (Robust Contrast Adaptive Sharpening).
// Licencia MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.
// Implementacion de referencia consultada:
//   https://github.com/GPUOpen-Effects/FidelityFX-FSR  (ffx_fsr1.h, FsrRcasH)
//   https://github.com/hrydgard/ppsspp/blob/master/assets/shaders/fsr_rcas.fsh
//
// Se toma de la fuente original de AMD, NO del PKG de GnmScaler (cuya licencia no esta declarada).
// El PKG sirvio para DESCUBRIR que RCAS es la pieza que faltaba, y su manifiesto lo confirma:
//   "unsharp a croce 5 tap con guadagno limitato per canale" / "scale: 1" (no reescala, solo realza)
//
// POR QUE SUSTITUYE AL UNSHARP SIMPLE
// -----------------------------------
// La version anterior aplicaba el MISMO realce en toda la imagen:
//
//   y' = y + (y - vecino) * k / 256
//
// Eso genera HALOS en los bordes de alto contraste: donde el gradiente es grande, el realce empuja
// el pixel mas alla del rango local y aparece un contorno claro. Es el compromiso que obligaba a
// dejar el realce suave.
//
// RCAS calcula el RANGO LOCAL (min y max de la cruz de 5 tomas) y LIMITA LA GANANCIA en funcion de
// ese rango:
//
//   zona plana  (rango pequeno) -> ganancia alta -> realza el detalle suave
//   borde fuerte (rango grande) -> ganancia baja -> NO genera halos
//
// Es decir: permite realzar MAS sin los halos. Es exactamente el compromiso que se buscaba.
//
// ADAPTACION A ENTEROS
// --------------------
// La version de AMD trabaja en coma flotante normalizada [0,1]. Aqui la luma es entero 0..255, asi
// que se sustituye la raiz inversa por una COTA ANALITICA de la ganancia, que es equivalente:
//
//   En AMD, con amp = 1/sqrt(clamp(...)), el termino de realce vale amp * K y el resultado se acota
//   a [0,1]. Traducido a enteros, la condicion para NO salirse del rango local es
//
//       |y - media| * ganancia <= min(espacio_arriba, espacio_abajo)
//
//   donde espacio_arriba = 255 - maxLocal y espacio_abajo = minLocal. Resolviendo para la ganancia
//   de un unsharp de la forma y' = y + (y - media) * g / 256:
//
//       g <= 256 * min(espacio_arriba, espacio_abajo) / |y - media|
//
//   Esto es una cota mas estricta que la de AMD (que permite cierto recorte controlado), y tiene la
//   ventaja de que el resultado NUNCA necesita saturacion: el realce queda garantizado dentro del
//   rango local, que es justo lo que evita los halos.
//
// COSTE Y POR QUE NO SE USAN LOS 4 VECINOS
// ----------------------------------------
// La version literal de RCAS lee los 4 vecinos de la cruz. Medido en consola, eso disparo el coste
// del escalado de 8523 us a 46886 us por frame (x5.5), porque las filas de arriba y abajo estan a
// 960 bytes del pixel actual y tiran la cache de datos: por cada pixel se tocan TRES lineas de
// cache distintas en lugar de una.
//
// Se conserva lo que da valor -la GANANCIA ADAPTATIVA por rango local, que es lo que elimina los
// halos- pero con 3 tomas: centro, arriba y abajo. Los vecinos verticales salen de la mezcla
// vertical que YA se habia calculado para este pixel, asi que estan en registros y no cuestan
// lecturas nuevas. Eso mantiene la adaptatividad donde mas importa (los bordes horizontales, que
// son los que dominan en texto e interfaz) a una fraccion del coste.
// ---------------------------------------------------------------------------
// TABLA DE RECIPROCOS: elimina la DIVISION POR PIXEL
//
// POR QUE: la version anterior calculaba `(room * 256) / adiff` para cada pixel. Una division
// entera en Jaguar cuesta del orden de 20-40 ciclos, y a 720p son 921.600 divisiones por frame:
// unos 28 millones de ciclos SOLO en dividir. Medido en consola, eso puso el escalado en
// dispatch_avg_us=28837 y los FPS en 24.
//
// COMO: en vez de dividir, se multiplica por el reciproco precalculado. La tabla cubre adiff de 1 a
// 256, que es todo el rango posible de una diferencia de luma; por encima de 256 el resultado de la
// comparacion es siempre "no limitar" y no hace falta tabla.
//
//   rcp[i] = 65536 / i        (punto fijo 16.16)
//   gmax   = (room * 256 * rcp[adiff]) >> 16
//
// La tabla son 257 entradas de uint32 (1 KB), construida una sola vez de forma perezosa.
// ---------------------------------------------------------------------------
namespace {
struct RcpTable {
    uint32_t v[257];
    RcpTable() {
        for (int i = 1; i < 257; ++i) v[i] = 65536u / static_cast<uint32_t>(i);
        v[0] = 0xFFFFFFFFu;   // adiff==0 no llega aqui: se sale antes por `if (diff == 0)`
    }
};
const RcpTable g_rcp;
}  // namespace

inline int rcas_luma_px(int center, int up, int down, int strength) {
    if (strength <= 0) return center;

    // Rango local de las 3 tomas (centro, arriba, abajo).
    int lo = center, hi = center;
    if (up < lo) lo = up;    if (up > hi) hi = up;
    if (down < lo) lo = down; if (down > hi) hi = down;

    // Media del rango local, por desplazamiento: (lo+hi)/2 en vez de (c+u+d)/3. Evita otra division
    // y es equivalente a efectos de realce: lo que importa es la desviacion respecto al centro del
    // rango, no la media aritmetica exacta.
    const int mean = (lo + hi) >> 1;

    const int diff = center - mean;
    if (diff == 0) return center;

    // Espacio disponible antes de salirse del rango local: es lo que limita los halos.
    const int roomUp = 255 - hi;
    const int roomDown = lo;
    const int room = (roomUp < roomDown) ? roomUp : roomDown;
    if (room <= 0) return center;                   // rango pegado al limite: no tocar

    const int adiff = (diff < 0) ? -diff : diff;

    // Ganancia maxima que mantiene el resultado dentro del rango local, SIN dividir:
    //   g <= 256 * room / adiff   ->   g <= (room * 256 * rcp[adiff]) >> 16
    int gmax;
    if (adiff <= 256) {
        gmax = static_cast<int>((static_cast<uint32_t>(room) * 256u * g_rcp.v[adiff]) >> 16);
    } else {
        gmax = 0;                                   // desviacion enorme: no realzar nada
    }

    // Ganancia pedida por el usuario.
    int g = strength;
    if (g > gmax) g = gmax;
    if (g <= 0) return center;

    int v = center + ((diff * g) >> 8);
    // La cota garantiza que no hace falta saturar, pero se comprueba igual por seguridad.
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}
// Toma el valor de luma ya mezclado en vertical para un indice de ORIGEN dado, y le aplica el
// realce horizontal si esta activo. Centralizarlo evita repetir la logica en los tres puntos de
// lectura (bloque SIMD, cola SIMD y camino escalar).
//
// El vecino se toma en el espacio de ORIGEN (sx - 1), NO en el de salida. La primera version usaba
// xi + 1 comparado con src_w, mezclando dos espacios de indices distintos: en la cola del bucle
// xi > src_w, asi que leia entradas del mapa que el relleno nunca alcanza, y el realce quedaba
// inconsistente segun la posicion. Trabajar en origen es correcto, y ademas el vecino es contiguo.
//
// RCAS necesita el rango local VERTICAL (arriba y abajo). Los dos valores salen de la mezcla
// vertical que ya se habia calculado para este pixel, asi que estan en registros: no cuestan
// lecturas nuevas ni tocan lineas de cache distintas. Ver la nota de coste en rcas_luma_px.
//
// NO se leen los vecinos horizontales: medido en consola, anadir esas dos lecturas (que caen a 960
// bytes de distancia sobre el plano de luma) disparo el coste del escalado x5.5. La ganancia
// adaptativa se mantiene con las 3 tomas verticales, que es donde estan los bordes dominantes en
// texto e interfaz.
inline int luma_at(const uint8_t* yrow, const uint8_t* yrow_b, int sx, int y_weight,
                   int sharpen_k, int src_h, int row_index, const uint8_t* y_plane, int y_pitch) {
    const int v = blend_luma_px(yrow[sx], yrow_b[sx], y_weight);
    if (sharpen_k <= 0) return v;

    // Vecino de arriba: la fila anterior del plano, con la MISMA mezcla vertical.
    int up = v;
    if (row_index > 0) {
        const uint8_t* yr = y_plane + static_cast<size_t>(row_index - 1) * y_pitch;
        const uint8_t* yrb = (yrow_b != yrow) ? (yr + y_pitch) : yr;
        up = blend_luma_px(yr[sx], yrb[sx], y_weight);
    }
    // Vecino de abajo. Si la mezcla vertical esta activa, la fila de abajo de la mezcla es la
    // SIGUIENTE a yrow, asi que se evita volver a calcular la direccion.
    int down = v;
    if (row_index + 1 < src_h) {
        const uint8_t* yrd = (yrow_b != yrow) ? (yrow_b + y_pitch)
                                             : (yrow + y_pitch);
        down = yrd[sx];
    }

    return rcas_luma_px(v, up, down, sharpen_k);
}

__attribute__((target("sse2")))
void scale_row_bgra_simd(uint8_t* dst, const uint8_t* yrow, const uint8_t* urow,
                         const uint8_t* vrow, const int* sx_map, const int* sux_map,
                         int dst_w, bool full_range, int store_mode,
                         const uint8_t* yrow_b, int y_weight, int src_w, int job_k,
                         int src_h, int row_index, const uint8_t* y_plane, int y_pitch) {
    const __m128i c_rv_mult = _mm_set1_epi16(full_range ? 202 : 230);
    const __m128i c_gu_mult = _mm_set1_epi16(full_range ? 24 : 27);
    const __m128i c_gv_mult = _mm_set1_epi16(full_range ? 60 : 68);
    const __m128i c_bu_mult = _mm_set1_epi16(full_range ? 238 : 270);
    // Alpha constant for bgra_store8.
    //
    // bgra_store8 feeds this to _mm_packus_epi16(g16, a255), which saturates each
    // 16-bit LANE down to one byte, so every int16 lane must hold 0x00FF to produce an
    // alpha byte of 0xFF. 0xFF000000 was wrong (that is lane 0x0000, packing to 0x00);
    // the host test caught it as "byte=3 simd=0 scalar=255" on every pixel.
    const __m128i a255 = _mm_set1_epi16(0x00FF);

    alignas(16) int16_t ybuf[8];
    alignas(16) int16_t rvbuf[8];
    alignas(16) int16_t guvbuf[8];
    alignas(16) int16_t bubuf[8];

    int x = 0;
    for (; x + 7 < dst_w; x += 8) {
        // Gather 8 output pixels and compute every scaled term in full int precision.
        // Chroma is subsampled horizontally (one U/V sample per two pixels), which
        // sux_map already encodes.
        for (int k = 0; k < 8; ++k) {
            const int xi = x + k;
            const int y_raw = (y_weight == 0 && job_k == 0)
                ? yrow[sx_map[xi]]
                : luma_at(yrow, yrow_b, sx_map[xi], y_weight, job_k, src_h, row_index, y_plane, y_pitch);
            const int uu = static_cast<int>(urow[sux_map[xi]]) - 128;
            const int vv = static_cast<int>(vrow[sux_map[xi]]) - 128;

            const int y_adj = full_range ? y_raw : (y_raw < 16 ? 0 : y_raw - 16);
            const int y_sc = ((full_range ? 128 : 149) * y_adj) >> 7;
            const int rv = ((full_range ? 202 : 230) * vv) >> 7;
            const int guv = ((full_range ? 24 : 27) * uu + (full_range ? 60 : 68) * vv) >> 7;
            const int bu = ((full_range ? 238 : 270) * uu) >> 7;

            ybuf[k] = static_cast<int16_t>(y_sc);
            rvbuf[k] = static_cast<int16_t>(rv);
            guvbuf[k] = static_cast<int16_t>(guv);
            bubuf[k] = static_cast<int16_t>(bu);
        }
        const __m128i y_sc = _mm_load_si128(reinterpret_cast<const __m128i*>(ybuf));
        const __m128i rv = _mm_load_si128(reinterpret_cast<const __m128i*>(rvbuf));
        const __m128i guv = _mm_load_si128(reinterpret_cast<const __m128i*>(guvbuf));
        const __m128i bu = _mm_load_si128(reinterpret_cast<const __m128i*>(bubuf));

        bgra_store8(dst + static_cast<size_t>(x) * 4, y_sc, rv, guv, bu, a255, store_mode);
    }
    // Tail pixels: dst_w is normally 1280/1920 (multiples of 8), but the tail must be
    // exact for any width.
    for (; x < dst_w; ++x) {
        const int yv = (y_weight == 0 && job_k == 0)
            ? yrow[sx_map[x]]
            : luma_at(yrow, yrow_b, sx_map[x], y_weight, job_k, src_h, row_index, y_plane, y_pitch);
        const int uu = static_cast<int>(urow[sux_map[x]]) - 128;
        const int vv = static_cast<int>(vrow[sux_map[x]]) - 128;
        yuv_to_bgra_px(dst + static_cast<size_t>(x) * 4, yv, uu, vv, full_range);
    }
}

// Scalar reference row conversion. Also used by the one-shot self-check below, so
// the escape hatch and the verification share exactly the same code.
//
// IMPORTANT: this must produce the SAME arithmetic as the SSE2 path, which is what
// yuv_to_bgra_px does NOT do. yuv_to_bgra_px applies a single >>7 to the whole
// expression - g = (y_scaled - 27*uu - 68*vv) >> 7 - whereas bgra_store8 consumes
// terms that were each shifted separately: g = (y_adj*149 >> 7) - ((27*uu + 68*vv) >> 7).
// Those differ by one for negative sums, because C's >> truncates toward negative
// infinity.
//
// This mattered: the console self-check reported "SIMD_MISMATCH" and fell back to the
// scalar path, but on inspection the SSE2 path was the one matching the documented
// formula and yuv_to_bgra_px was the outlier. Comparing SIMD against a reference that
// uses a different formula produced a false failure and disabled the fast path.
void scale_row_bgra_scalar(uint8_t* dst, const uint8_t* yrow, const uint8_t* urow,
                           const uint8_t* vrow, const int* sx_map, const int* sux_map,
                           int dst_w, bool full_range,
                           const uint8_t* yrow_b, int y_weight, int src_w, int job_k,
                         int src_h, int row_index, const uint8_t* y_plane, int y_pitch) {
    const int y_mult = full_range ? 128 : 149;
    const int rv_mult = full_range ? 202 : 230;
    const int gu_mult = full_range ? 24 : 27;
    const int gv_mult = full_range ? 60 : 68;
    const int bu_mult = full_range ? 238 : 270;
    for (int x = 0; x < dst_w; ++x) {
        // Se aplica la misma mezcla vertical y el mismo realce que el camino SIMD: si los dos
        // caminos no trataran la luma igual, la autocomprobacion los veria distintos y caeria al
        // escalar creyendo que el rapido esta roto.
        const int y_raw = (y_weight == 0 && job_k == 0)
            ? yrow[sx_map[x]]
            : luma_at(yrow, yrow_b, sx_map[x], y_weight, job_k, src_h, row_index, y_plane, y_pitch);
        const int uu = static_cast<int>(urow[sux_map[x]]) - 128;
        const int vv = static_cast<int>(vrow[sux_map[x]]) - 128;

        const int y_adj = full_range ? y_raw : (y_raw < 16 ? 0 : y_raw - 16);
        const int y_sc = (y_mult * y_adj) >> 7;
        const int rv = (rv_mult * vv) >> 7;
        const int guv = (gu_mult * uu + gv_mult * vv) >> 7;
        const int bu = (bu_mult * uu) >> 7;

        uint8_t* dp = dst + static_cast<size_t>(x) * 4;
        dp[0] = clamp_u8(y_sc + bu);
        dp[1] = clamp_u8(y_sc - guv);
        dp[2] = clamp_u8(y_sc + rv);
        dp[3] = 0xFF;
    }
}

// Test hook for the host benchmark: how many participants the pool has.
int RowWorkerPoolProbe() { return RowWorkerPool::instance().thread_count(); }

void scale_rows_yuv420(int y_begin, int y_end) {
    const ScaleJob& job = g_scale_job;
    for (int y = y_begin; y < y_end; ++y) {
        uint8_t* drow = job.dst_bgra + static_cast<size_t>(y) * job.dst_pitch;
        int vw = 0;
        const int sy = map_source_row_weighted(y, job, vw);
        int su = sy / 2;
        if (su >= job.src_h / 2) su = (job.src_h / 2) - 1;
        if (su < 0) su = 0;

        const uint8_t* yrow = job.y_plane + static_cast<size_t>(sy) * job.y_pitch;
        // Mezcla vertical con la fila siguiente, si hay buffer y no es la ultima fila.
        // Segunda fila para la mezcla vertical. Si no hay escalado vertical, apunta a la misma
        // fila y el peso es 0, con lo que blend_luma_px devuelve el valor original sin coste.
        const uint8_t* yrow_b = job.y_vertical_blend ? (yrow + job.y_pitch) : yrow;
        const int y_weight_eff = job.y_vertical_blend ? vw : 0;
        const uint8_t* urow = job.u_plane + static_cast<size_t>(su) * job.u_pitch;
        const uint8_t* vrow = job.v_plane + static_cast<size_t>(su) * job.v_pitch;

        if (!job.scalar_fallback) {
            scale_row_bgra_simd(drow, yrow, urow, vrow, job.sx_map, job.sux_map,
                                job.dst_w, job.full_range, job.store_mode, yrow_b, y_weight_eff,
                                job.src_w, job.sharpen_k,
                                job.src_h, sy, job.y_plane, job.y_pitch);
            continue;
        }
        scale_row_bgra_scalar(drow, yrow, urow, vrow, job.sx_map, job.sux_map,
                              job.dst_w, job.full_range, yrow_b, y_weight_eff,
                              job.src_w, job.sharpen_k,
                                job.src_h, sy, job.y_plane, job.y_pitch);
    }
}

void scale_rows_nv12(int y_begin, int y_end) {
    const ScaleJob& job = g_scale_job;
    for (int y = y_begin; y < y_end; ++y) {
        uint8_t* drow = job.dst_bgra + static_cast<size_t>(y) * job.dst_pitch;
        int vw = 0;
        const int sy = map_source_row_weighted(y, job, vw);
        int su = sy / 2;
        if (su >= job.src_h / 2) su = (job.src_h / 2) - 1;
        if (su < 0) su = 0;

        const uint8_t* yrow = job.y_plane + static_cast<size_t>(sy) * job.y_pitch;
        // Segunda fila para la mezcla vertical (ver blend_luma_px), sin buffer compartido.
        const uint8_t* yrow_b = job.y_vertical_blend ? (yrow + job.y_pitch) : yrow;
        const int y_weight_eff = job.y_vertical_blend ? vw : 0;
        const uint8_t* uvrow = job.uv_plane + static_cast<size_t>(su) * job.uv_pitch;

        for (int x = 0; x < job.dst_w; ++x) {
            const int sx = job.sx_map[x];
            const int sux = job.sux_map[x];
            const int y_val = blend_luma_px(yrow[sx], yrow_b[sx], y_weight_eff);
            const int uu = static_cast<int>(uvrow[sux * 2]) - 128;
            const int vv = static_cast<int>(uvrow[sux * 2 + 1]) - 128;
            yuv_to_bgra_px(drow + static_cast<size_t>(x) * 4, y_val, uu, vv, job.full_range);
        }
    }
}

// Ajuste global del realce de nitidez. Ver la declaracion en color_simd.hpp.
//
// Se guarda como variable atomica porque la leen los hilos del pool mientras el hilo principal la
// escribe. El valor por defecto (100) es el realce base calculado a partir del factor de escala.
static std::atomic<int> g_sharpen_percent{100};

void SetLumaSharpenPercent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 400) percent = 400;
    g_sharpen_percent.store(percent, std::memory_order_relaxed);
}
void ScaleBilinearYUV420PToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int dst_w, int dst_h,
    int src_w, int src_h,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* u_plane, int u_pitch,
    const uint8_t* v_plane, int v_pitch,
    bool full_range) {
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    // Horizontal sample indices depend only on the source/target width pair, so
    // they survive across frames at the same input resolution.
    static std::vector<int> sx_map;
    static std::vector<int> sux_map;
    static int cached_dst_w = -1;
    static int cached_src_w = -1;
    if (cached_dst_w != dst_w || cached_src_w != src_w) {
        sx_map.resize(dst_w);
        sux_map.resize(dst_w);
        const uint32_t x_ratio = ((static_cast<uint32_t>(src_w) << 16) / dst_w);
        for (int x = 0; x < dst_w; x++) {
            int sx = static_cast<int>((x * x_ratio) >> 16);
            if (sx >= src_w) sx = src_w - 1;
            if (sx < 0) sx = 0;
            sx_map[x] = sx;
            int sux = sx / 2;
            if (src_w / 2 > 0 && sux >= src_w / 2) sux = (src_w / 2) - 1;
            if (sux < 0) sux = 0;
            sux_map[x] = sux;
        }
        cached_dst_w = dst_w;
        cached_src_w = src_w;
    }

    ScaleJob& job = g_scale_job;
    job.dst_bgra = dst_bgra;
    job.dst_pitch = dst_pitch;
    job.dst_w = dst_w;
    job.src_w = src_w;
    job.src_h = src_h;
    job.y_plane = y_plane;
    job.u_plane = u_plane;
    job.v_plane = v_plane;
    job.y_pitch = y_pitch;
    job.u_pitch = u_pitch;
    job.v_pitch = v_pitch;
    job.full_range = full_range;
    job.sx_map = sx_map.data();
    job.sux_map = sux_map.data();
    job.y_ratio = ((static_cast<uint32_t>(src_h) << 16) / dst_h);
    job.yuv420 = true;

    // Mezcla VERTICAL de luma (ver blend_luma_px): solo tiene sentido si hay escalado vertical.
    job.y_vertical_blend = (dst_h != src_h);
    // Realce proporcional al factor de escala: quanto mas amplia, mas contraste se recupera.
    // A 1.33x (540p->720p) sale k=40; a 2x (540p->1080p) sale k=64. Se acota a [0,72].
    job.sharpen_k = 0;
    if (dst_h > src_h && src_h > 0) {
        // Base proporcional al factor de escala, ajustada por el porcentaje del usuario.
        {
            int base_k = ((dst_h * 256) / src_h - 256) / 3;
            if (base_k < 0) base_k = 0;
            if (base_k > 72) base_k = 72;
            const int pct = g_sharpen_percent.load(std::memory_order_relaxed);
            job.sharpen_k = (base_k * pct) / 100;
            if (job.sharpen_k > 128) job.sharpen_k = 128;   // limite: mas son halos
        }
        if (job.sharpen_k < 0) job.sharpen_k = 0;
        if (job.sharpen_k > 72) job.sharpen_k = 72;
    }

    // One-shot correctness self-check for the SSE2 scaled row converter.
    //
    // Reasoning carefully about SIMD vs scalar is exactly the kind of thinking that
    // has been wrong repeatedly in this project, so the two paths are compared on
    // identical input instead. Runs ONCE, on the calling thread before the pool is
    // dispatched (never inside a worker, so there is no race).
    //
    // TOLERANCE: exact. Host verification (tests/scale_simd_host_test.cpp, 400 cases
    // across 10 geometries including non-multiples of 8) reports 0 failures and shows
    // the SSE2 path is byte-identical to the scalar reference. There is no rounding
    // difference left because every multiply and shift is now done in full int width.
    //
    // This check exists because reasoning about SIMD correctness was wrong repeatedly
    // during this work: it silently produced black pixels at high Y and a zero alpha
    // channel, both only visible on the host test. If a future change makes the two
    // paths disagree, the scalar path is used instead and the mismatch is logged, so a
    // broken fast path can never reach the screen unnoticed.
    static bool s_simd_verified = false;
    job.scalar_fallback = false;
    uint64_t check_us = 0;
    if (!s_simd_verified && src_w > 0 && src_h > 0) {
        const uint64_t check_t0 = scale_now_us();
        s_simd_verified = true;
        const int prove_rows = std::min(2, src_h);
        bool differ = false;
        int first_diff_x = -1, first_diff_byte = -1;
        for (int ry = 0; ry < prove_rows && !differ; ++ry) {
            const uint8_t* yrow = y_plane + static_cast<size_t>(ry) * y_pitch;
            const uint8_t* urow = u_plane + static_cast<size_t>(ry / 2) * u_pitch;
            const uint8_t* vrow = v_plane + static_cast<size_t>(ry / 2) * v_pitch;
            alignas(16) static uint8_t simd_probe[8192];
            alignas(16) static uint8_t scalar_probe[8192];
            const int probe_w = std::min(dst_w, static_cast<int>(sizeof(simd_probe) / 4));
            std::memset(simd_probe, 0, sizeof(simd_probe));
            std::memset(scalar_probe, 0, sizeof(scalar_probe));
            scale_row_bgra_simd(simd_probe, yrow, urow, vrow, sx_map.data(), sux_map.data(),
                                probe_w, full_range, STORE_UNALIGNED, yrow, 0, probe_w, 0,
                                  0, 0, yrow, probe_w);
            scale_row_bgra_scalar(scalar_probe, yrow, urow, vrow, sx_map.data(), sux_map.data(),
                                  probe_w, full_range, yrow, 0, probe_w, 0,
                                    0, 0, yrow, probe_w);
            for (int i = 0; i < probe_w * 4; ++i) {
                if (simd_probe[i] != scalar_probe[i]) {
                    differ = true;
                    first_diff_byte = i;
                    first_diff_x = i / 4;
                    break;
                }
            }
        }
        if (differ) {
            job.scalar_fallback = true;
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "SIMD_MISMATCH x=%d byte=%d -> using scalar path",
                          first_diff_x, first_diff_byte);
            LogAppLifecycleEvent("VIDEOOUT_SCALE_SIMD_SELFCHECK", detail);
        } else {
            char detail[128];
            std::snprintf(detail, sizeof(detail),
                          "ok rows=%d width=%d simd_byte_identical=1", prove_rows, dst_w);
            LogAppLifecycleEvent("VIDEOOUT_SCALE_SIMD_SELFCHECK", detail);
        }
        check_us = scale_now_us() - check_t0;
    }

    // Scale timings, reported by the caller so the console can show exactly where the
    // cost is. Added because the scaled path measured 560 ms/frame on console while this
    // same call measures ~5 ms on the host: the difference has to be visible in one of
    // these numbers, and until it is, every explanation is a guess.
    static uint64_t s_check_total_us = 0;
    static uint64_t s_dispatch_total_us = 0;
    static uint32_t s_timed_frames = 0;

    s_check_total_us += check_us;
    const uint64_t dispatch_t0 = scale_now_us();
    RowWorkerPool::instance().run(&scale_rows_yuv420, dst_h);
    s_dispatch_total_us += scale_now_us() - dispatch_t0;
    ++s_timed_frames;
    if (s_timed_frames % 60 == 0) {
        char timingDetail[224];
        std::snprintf(timingDetail, sizeof(timingDetail),
                      "frames=%u check_avg_us=%llu dispatch_avg_us=%llu threads=%d dst=%dx%d",
                      s_timed_frames,
                      static_cast<unsigned long long>(s_check_total_us / 60),
                      static_cast<unsigned long long>(s_dispatch_total_us / 60),
                      RowWorkerPoolProbe(), dst_w, dst_h);
        LogAppLifecycleEvent("VIDEOOUT_SCALE_TIMING", timingDetail);
        s_check_total_us = 0;
        s_dispatch_total_us = 0;
    }
}

void ScaleBilinearNV12ToBGRA_BT709(
    uint8_t* dst_bgra, int dst_pitch,
    int dst_w, int dst_h,
    int src_w, int src_h,
    const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    bool full_range) {
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    static std::vector<int> sx_map;
    static std::vector<int> sux_map;
    static int cached_dst_w = -1;
    static int cached_src_w = -1;
    if (cached_dst_w != dst_w || cached_src_w != src_w) {
        sx_map.resize(dst_w);
        sux_map.resize(dst_w);
        const uint32_t x_ratio = ((static_cast<uint32_t>(src_w) << 16) / dst_w);
        for (int x = 0; x < dst_w; x++) {
            int sx = static_cast<int>((x * x_ratio) >> 16);
            if (sx >= src_w) sx = src_w - 1;
            if (sx < 0) sx = 0;
            sx_map[x] = sx;
            // Byte offset of the interleaved UV pair for this source column.
            int sux = (sx / 2) * 2;
            const int uv_limit = (src_w & ~1) - 2;
            if (uv_limit >= 0 && sux > uv_limit) sux = uv_limit;
            if (sux < 0) sux = 0;
            sux_map[x] = sux;
        }
        cached_dst_w = dst_w;
        cached_src_w = src_w;
    }

    ScaleJob& job = g_scale_job;
    job.dst_bgra = dst_bgra;
    job.dst_pitch = dst_pitch;
    job.dst_w = dst_w;
    job.src_w = src_w;
    job.src_h = src_h;
    job.y_plane = y_plane;
    job.uv_plane = uv_plane;
    job.y_pitch = y_pitch;
    job.uv_pitch = uv_pitch;
    job.full_range = full_range;
    job.sx_map = sx_map.data();
    job.sux_map = sux_map.data();
    job.y_ratio = ((static_cast<uint32_t>(src_h) << 16) / dst_h);
    job.yuv420 = false;
    // Igual que en el camino YUV420.
    job.y_vertical_blend = (dst_h != src_h);
    // Realce proporcional al factor de escala: quanto mas amplia, mas contraste se recupera.
    // A 1.33x (540p->720p) sale k=40; a 2x (540p->1080p) sale k=64. Se acota a [0,72].
    job.sharpen_k = 0;
    if (dst_h > src_h && src_h > 0) {
        // Base proporcional al factor de escala, ajustada por el porcentaje del usuario.
        {
            int base_k = ((dst_h * 256) / src_h - 256) / 3;
            if (base_k < 0) base_k = 0;
            if (base_k > 72) base_k = 72;
            const int pct = g_sharpen_percent.load(std::memory_order_relaxed);
            job.sharpen_k = (base_k * pct) / 100;
            if (job.sharpen_k > 128) job.sharpen_k = 128;   // limite: mas son halos
        }
        if (job.sharpen_k < 0) job.sharpen_k = 0;
        if (job.sharpen_k > 72) job.sharpen_k = 72;
    }

    RowWorkerPool::instance().run(&scale_rows_nv12, dst_h);
}

} // namespace opennow::color

// Test hook: reports how many participants the scaler pool actually has (workers + the
// calling thread). Exposed so the host benchmark can prove whether threading is in play,
// because "the pool silently has no workers" would explain a large slowdown that no
// per-function timing would reveal.
extern "C" int RowWorkerPoolProbe() {
    return opennow::color::RowWorkerPoolProbe();
}
