// Command-line driver for the unmodified k7zx 4.3 conversion engine
// (original/zxwav.cpp, ZXCODE.CPP, rutinas.cpp, zxfiles.cpp), built by
// build.sh.  It sets the engine's globals the way k7zx 4.3's GUI does and
// calls convierteHI(), so its WAVs are what k7zx 4.3 itself would write.
//
// usage: k7zx43 in.tap out.wav METHOD SPB RATE SCHEME [WAVE] [CHECKSUM]
//   METHOD   0..18, zxwav.h numbering (= k7zx::Method)
//   SPB      zxwav.h speed enum (= k7zx::SamplesPerBit, e.g. 11 for 2.75)
//   RATE     44100 or 48000
//   SCHEME   1 one block, 2 many blocks, 3 original loader (the request;
//            PlayerForm.cpp's substitution rule is applied, as in 4.3)
//   WAVE     forma_onda, default 2 (cubic)
//   CHECKSUM tape-error check box, default 1
#include <stdio.h>
#include <stdlib.h>

#include "zxfiles.h"
#include "zxwav.h"
#include "ZXCODE.h"
#include "rutinas.h"
int readzxfile(const char* filename);
int convierteHI(const char* out_file);

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: k7zx43 in out.wav METHOD SPB RATE SCHEME [WAVE] [CHECKSUM]\n");
        return 2;
    }
    const int m = atoi(argv[3]), spb = atoi(argv[4]), rate = atoi(argv[5]), sch = atoi(argv[6]);
    const int wave = argc > 7 ? atoi(argv[7]) : 2;
    const int chk = argc > 8 ? atoi(argv[8]) : 1;
    if (readzxfile(argv[1])) {
        fprintf(stderr, "k7zx43: cannot read %s: %s\n", argv[1], m_errors);
        return 1;
    }
    // PlayerForm.cpp: the original loader is Milks-only, and Shavings Raudo
    // never uses many blocks.
    int esq;
    if (sch == 3 && m == 1) esq = 3;
    else if ((sch == 3 || sch == 2) && m != 5) esq = 2;
    else esq = 1;
    metodo = (char)m;
    muestras_por_bit = (char)spb;
    FqMuestreo = rate;
    forma_onda = (char)wave;
    signo = 1; tono_final = 0; CD = 0; inv_r = 1; acelerar_basic = 0;
    divisor = 1; multipicador = 1; max_vol = 0; emular = 0;
    cargador = 1; antikolmogorov = 0;
    esquema_bloques = (char)esq;
    // SettingdForm.cpp clears the check box for these techniques.
    control_chksum = chk && !(m == 0 || m == 4 || m == 5 || m == 6 || m == 9 || m == 12);
    dir_clear = clearN ? clearN : 65344;  // MainForm.cpp: if (dir_clear==0) dir_clear=65344
    dir_usr = usrN;
    pokes[0].dir = 0;
    const int r = convierteHI(argv[2]);
    if (r) fprintf(stderr, "k7zx43: convierteHI returned %d: %s\n", r, m_errors);
    return r;
}
