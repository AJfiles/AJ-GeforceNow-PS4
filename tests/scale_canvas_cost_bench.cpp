// =================================================================================================
// Banco: coste del ESCALADO que introdujo la v3.53
// =================================================================================================
// POR QUE ESTE BANCO
// ---------------------------------------------------------------------------------------------
// La v3.53 cambio la ruta del video para que la textura se cree **al tamano del lienzo** (1920x1080)
// en lugar del tamano del frame (1280x720). El motivo era la nitidez: SDL escalaba con
// `SDL_BlitScaled`, que usa VECINO MAS CERCANO y rompia el texto.
//
// A cambio, el escalado bilineal pasa a hacerlo NUESTRO codigo, y **eso cuesta CPU**. La pregunta
// obvia es: ¿cuanto?
//
// El banco que ya existia (`scale_path_bench.cpp`) mide **960x540 -> 1280x720**, que era el caso
// antiguo. Este mide los DOS casos que importan ahora:
//
//     A) 1280x720 -> 1920x1080   (destino = lienzo de 1080p)   <- lo que hace la v3.53
//     B) 1280x720 -> 1280x720    (sin escalar, solo conversion) <- lo que haria un lienzo de 720p
//
// **La diferencia entre A y B es el precio exacto de la nitidez**, y el dato que decide si el
// escalado bilineal compensa o si hay que bajar el lienzo.
//
// ---------------------------------------------------------------------------------------------
// COMO INTERPRETAR EL RESULTADO
// ---------------------------------------------------------------------------------------------
// El PC de desarrollo tiene nucleos mucho mas rapidos que la CPU Jaguar de la PS4. La nota del banco
// antiguo decia que la consola midio **560 ms/frame** en un caso que aqui daba **11 ms**: un factor de
// ~50x. Ese factor incluye que el PC tenia solo 2 hilos en el pool mientras la PS4 usa mas.
//
// **Con esa incertidumbre, este banco NO sirve para predecir el numero de la PS4.** Sirve para algo
// mas util y mas seguro: **la RAZON entre A y B**. Esa razon depende del numero de pixeles de salida
// (2.073.600 / 921.600 = 2,25x) y **se traslada a la consola**, porque el trabajo por pixel es el
// mismo en las dos.
//
// Si en el PC A tarda ~2,25x mas que B, el modelo es correcto y en la PS4 pasara lo mismo. Si la
// razon es mucho mayor, hay algo mas caro que el trabajo por pixel (por ejemplo, mas fallos de cache
// al escribir mas superficie) y el coste en consola seria peor de lo previsto.
// =================================================================================================

#include "../src/opennow/stream/color_simd.cpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace opennow::color;

// `color_simd.cpp` llama a `opennow::LogAppLifecycleEvent` en su instrumentacion. En un binario de host
// esa funcion no existe (vive en el logger del cliente, que escribe en /data del PS4), asi que se
// proporciona un stub vacio para poder enlazar sin arrastrar todo el cliente.
namespace opennow {
void LogAppLifecycleEvent(const char*, const char*) {}
}

static double bench(int src_w, int src_h, int dst_w, int dst_h, int iters) {
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> dist(0, 255);

    // Fuente en NV12: plano Y completo + plano UV intercalado.
    std::vector<uint8_t> plane_y(static_cast<size_t>(src_w) * src_h);
    std::vector<uint8_t> plane_uv(static_cast<size_t>(src_w) * (src_h / 2));
    for (auto& b : plane_y) b = static_cast<uint8_t>(dist(rng));
    for (auto& b : plane_uv) b = static_cast<uint8_t>(dist(rng));

    std::vector<uint8_t> dst(static_cast<size_t>(dst_w) * dst_h * 4, 0);

    // Calentamiento: la primera llamada hace ademas la autocomprobacion SIMD y construye los mapas
    // de filas de origen, que despues se reutilizan entre frames del mismo tamano.
    ScaleBilinearNV12ToBGRA_BT709(dst.data(), dst_w * 4, dst_w, dst_h, src_w, src_h,
                                  plane_y.data(), src_w, plane_uv.data(), src_w, false);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        ScaleBilinearNV12ToBGRA_BT709(dst.data(), dst_w * 4, dst_w, dst_h, src_w, src_h,
                                      plane_y.data(), src_w, plane_uv.data(), src_w, false);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

int main() {
    const int iters = 40;

    std::printf("\n  COSTE DEL ESCALADO QUE INTRODUJO LA v3.53\n\n");
    // NOTA: hay DOS `RowWorkerPoolProbe` (la de dentro del espacio de nombres y una version
    // `extern "C"` al final del fichero), asi que sin cualificar la llamada es ambigua.
    std::printf("  pool: %d participantes\n\n", opennow::color::RowWorkerPoolProbe());

    const double a = bench(1280, 720, 1920, 1080, iters);   // destino = lienzo 1080p
    const double b = bench(1280, 720, 1280, 720,  iters);   // sin escalar (solo conversion)

    std::printf("  A) 1280x720 -> 1920x1080 (textura del lienzo 1080p) : %8.3f ms/frame\n", a);
    std::printf("  B) 1280x720 -> 1280x720  (sin escalar)              : %8.3f ms/frame\n", b);
    std::printf("\n");

    if (b > 0.0) {
        std::printf("  RAZON A/B = %.2f   (pixeles de salida: 2,25x)\n", a / b);
        if ((a / b) < 2.6) {
            std::printf("  -> La razon sigue el numero de pixeles de salida: el coste es trabajo por pixel.\n");
            std::printf("     Se traslada a la consola de forma proporcional.\n");
        } else {
            std::printf("  -> La razon SUPERA el numero de pixeles de salida: hay coste extra ademas\n");
            std::printf("     del trabajo por pixel (probablemente memoria/cache al escribir mas superficie).\n");
            std::printf("     En la consola el coste seria PEOR de lo que sugiere la proporcion.\n");
        }
    }
    std::printf("\n");
    return 0;
}
