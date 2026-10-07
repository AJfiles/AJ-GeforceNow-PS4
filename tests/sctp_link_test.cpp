// Prueba de enlace de libusrsctp.a contra el toolchain de Orbis.
//
// POR QUE EXISTE
// --------------
// El informe de viabilidad de xCloud identifico que la documentacion del proyecto
// (docs/PS4_GFN_VIDEO_PIPELINE.md, linea 38) afirma que "usrsctp aun no compila por diferencias de
// los headers BSD de Orbis". Pero el artefacto libusrsctp.a EXISTE y esta compilado con -D__ORBIS__.
//
// Eso importa mucho para xCloud: xCloud manda la ENTRADA DEL MANDO por data channels SCTP.
// GeForce NOW no usa SCTP (usa NVST), por eso nunca lo necesito de verdad. Si usrsctp enlaza, la
// viabilidad de xCloud sube varios puntos; si no enlaza, xCloud no tiene entrada de mando.
//
// Esta prueba NO toca la red: solo toma las direcciones de las funciones para forzar al enlazador
// a resolver todos los simbolos. Si enlaza, la libreria es utilizable tal cual.
//
// Se usan SOLO cabeceras de C (no libc++) para que la prueba no dependa de la configuracion de
// libc++ de Orbis: lo unico que se quiere medir es si usrsctp enlaza.
#include <stdio.h>

extern "C" {

// La libreria declara sus tipos en sus propias cabeceras; para esta prueba bastan punteros
// opacos, porque no se llama a nada. Lo unico que importa es que el ENLAZADOR encuentre el
// simbolo, y eso depende del nombre, no de la firma.
void usrsctp_init(unsigned short port);
void usrsctp_finish(void);
void* usrsctp_socket(int domain, int type, int protocol, void*, void*, unsigned int, void*);
int usrsctp_bind(void*, void*, unsigned int);
int usrsctp_connect(void*, void*, unsigned int);
int usrsctp_close(void*);
int usrsctp_sendv(void*, const void*, unsigned long, void*, int, void*, void*, void*, int);
int usrsctp_recvv(void*, void*, unsigned long, void*, unsigned int*, void*, unsigned long*, void*, int*);
int usrsctp_listen(void*, int);
void* usrsctp_accept(void*, void*, unsigned int*);

}  // extern "C"

int main() {
    // Se toman las direcciones: obliga al enlazador a resolver cada simbolo.
    const void* syms[] = {
        reinterpret_cast<const void*>(&usrsctp_init),
        reinterpret_cast<const void*>(&usrsctp_finish),
        reinterpret_cast<const void*>(&usrsctp_socket),
        reinterpret_cast<const void*>(&usrsctp_bind),
        reinterpret_cast<const void*>(&usrsctp_connect),
        reinterpret_cast<const void*>(&usrsctp_close),
        reinterpret_cast<const void*>(&usrsctp_sendv),
        reinterpret_cast<const void*>(&usrsctp_recvv),
        reinterpret_cast<const void*>(&usrsctp_listen),
        reinterpret_cast<const void*>(&usrsctp_accept),
    };
    const int n = static_cast<int>(sizeof(syms) / sizeof(syms[0]));

    FILE* f = fopen("/data/gfnps4/sctp_link_test.txt", "w");
    if (f) {
        fprintf(f, "USRSCTP_LINK_TEST resolved=%d\n", n);
        for (int i = 0; i < n; ++i) {
            fprintf(f, "  symbol[%d]=%p\n", i, syms[i]);
        }
        fclose(f);
    }
    return 0;
}
