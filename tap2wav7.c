#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SAMPLE_RATE 7350

typedef struct {
    char chunk_id[4];
    unsigned int chunk_size;
    char format[4];
    char subchunk1_id[4];
    unsigned int subchunk1_size;
    unsigned short audio_format;
    unsigned short num_channels;
    unsigned int sample_rate;
    unsigned int byte_rate;
    unsigned short block_align;
    unsigned short bits_per_sample;
    char subchunk2_id[4];
    unsigned int subchunk2_size;
} WavHeader;

typedef struct {
    unsigned char *data;
    unsigned int size;
    unsigned int capacity;
} AudioBuffer;

void init_audio_buffer(AudioBuffer *buf) {
    buf->capacity = SAMPLE_RATE * 30;
    buf->data = (unsigned char *)malloc(buf->capacity);
    buf->size = 0;
}

void append_sample(AudioBuffer *buf, unsigned char sample) {
    if (buf->size >= buf->capacity) {
        buf->capacity *= 2;
        buf->data = (unsigned char *)realloc(buf->data, buf->capacity);
    }
    buf->data[buf->size++] = sample;
}

void append_exact_bit(AudioBuffer *buf, int bit_value) {
    if (bit_value == 0) {
        for(int i=0; i<3; i++) append_sample(buf, 255);
        for(int i=0; i<3; i++) append_sample(buf, 0);
    } else {
        for(int i=0; i<2; i++) append_sample(buf, 255);
        for(int i=0; i<1; i++) append_sample(buf, 0);
        for(int i=0; i<1; i++) append_sample(buf, 255);
        for(int i=0; i<2; i++) append_sample(buf, 0);
    }
}

void append_exact_leader(AudioBuffer *buf, float seconds) {
    int total_samples = (int)(SAMPLE_RATE * seconds);
    int num_pairs = total_samples / 12;
    for (int i = 0; i < num_pairs; i++) {
        for(int j=0; j<4; j++) append_sample(buf, 255);
        for(int j=0; j<2; j++) append_sample(buf, 0);
        for(int j=0; j<2; j++) append_sample(buf, 255);
        for(int j=0; j<4; j++) append_sample(buf, 0);
    }
}

void encode_byte_msb(AudioBuffer *buf, unsigned char byte) {
    // 1. Start bit a uno (Lógica FSK invertida / Bin2Wave)
    append_exact_bit(buf, 1);
    // 2. Datos MSB First (Del bit 7 al bit 0)
    for (int i = 7; i >= 0; i--) {
        append_exact_bit(buf, (byte >> i) & 1);
    }
    // 3. 1 Stop bit a cero
    append_exact_bit(buf, 0);
}

bool generate_exact_wav_from_tap(const char *input_path, const char *output_path) {
    FILE *f_in = fopen(input_path, "rb");
    if (!f_in) {
        printf("Error: No se pudo abrir el archivo TAP '%s'\n", input_path);
        return false;
    }

    fseek(f_in, 0, SEEK_END);
    long file_size = ftell(f_in);
    fseek(f_in, 0, SEEK_SET);

    unsigned char *tap_payload = (unsigned char *)malloc(file_size);
    size_t read_bytes = fread(tap_payload, 1, file_size, f_in);
    fclose(f_in);

    if (read_bytes == 0) {
        free(tap_payload);
        return false;
    }

    AudioBuffer audio_buf;
    init_audio_buffer(&audio_buf);

    long offset = 0;
    int block_index = 1;

    printf("Decodificando bloques TAP nativos y modulando en FSK...\n");

    while (offset < file_size) {
        // Localizar el byte 0x00 inicial de sincronismo de bloque
        if (tap_payload[offset] != 0x00) {
            offset++;
            continue;
        }

        if (offset + 3 > file_size) break;

        // Lectura Little Endian real de los bytes de longitud del bloque
        unsigned int data_len = tap_payload[offset + 1] | (tap_payload[offset + 2] << 8);
        
        // Estructura: 1 Sync + 2 Len + data_len + 1 Type + 2 Checksum + 9 Zeros = 15 + data_len
        unsigned int total_block_bytes = 15 + data_len;

        if (offset + total_block_bytes > file_size) {
            printf("Aviso: Bloque %d truncado o fin de archivo inesperado.\n", block_index);
            break;
        }

        // Posición exacta del Block Type
        unsigned char block_type = tap_payload[offset + 3 + data_len];

        float leader_time = 2.0f; 
        if (block_type == 0x00) {
            leader_time = 3.0f;
            printf(" -> Bloque %d: Cabecera (Type: 0x00, Longitud Payload: 0x%04X [%u]). Guía: 3.0s\n", block_index, data_len, data_len);
        } else if (block_type == 0x55) {
            leader_time = 1.0f;
            printf(" -> Bloque %d: Fin de Archivo EOF (Type: 0x55, Longitud Payload: 0x%04X [%u]). Guía: 1.0s\n", block_index, data_len, data_len);
        } else {
            printf(" -> Bloque %d: Segmento Datos (Type: 0x%02X, Longitud Payload: 0x%04X [%u]). Guía: 2.0s\n", block_index, block_type, data_len, data_len);
        }

        // A. Modular el Tono Guía (Leader) del bloque correspondiente
        append_exact_leader(&audio_buf, leader_time);

        // B. Modular todos los bytes del bloque físico (incluyendo el Sync, la longitud y los 9 ceros finales)
        for (unsigned int i = 0; i < total_block_bytes; i++) {
            encode_byte_msb(&audio_buf, tap_payload[offset + i]);
        }

        // C. Pausa/Gap físico de 2 segundos (silencio plano a nivel neutro 128) justo antes de pasar al siguiente bloque
        int gap_samples = (int)(SAMPLE_RATE * 2.0f);
        for (int i = 0; i < gap_samples; i++) {
            append_sample(&audio_buf, 128);
        }

        offset += total_block_bytes;
        block_index++;
    }

    free(tap_payload);

    FILE *f_wav = fopen(output_path, "wb");
    if (!f_wav) {
        free(audio_buf.data);
        return false;
    }

    WavHeader header;
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 36 + audio_buf.size;
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = SAMPLE_RATE;
    header.byte_rate = SAMPLE_RATE;
    header.block_align = 1;
    header.bits_per_sample = 8;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = audio_buf.size;

    fwrite(&header, 1, 44, f_wav);
    fwrite(audio_buf.data, 1, audio_buf.size, f_wav);
    
    fclose(f_wav);
    free(audio_buf.data);
    printf("\n[ÉXITO] Archivo WAV generado correctamente en: '%s'\n", output_path);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <archivo.tap>\n", argv);
        return 1;
    }
    char *input_file = argv[1];

    char *output_wav = (char *)malloc(strlen(input_file) + 5);
    strcpy(output_wav, input_file);
    char *ext_ptr = strrchr(output_wav, '.');
    if (ext_ptr) strcpy(ext_ptr, ".wav");
    else strcat(output_wav, ".wav");

    generate_exact_wav_from_tap(input_file, output_wav);
    free(output_wav);
    return 0;
}

