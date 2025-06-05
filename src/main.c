#include <stdio.h>
#include <string.h>
#include <inttypes.h> 

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h> 

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <ff.h>
#define DISK_DRIVE_NAME  "SD"
#define DISK_MOUNT_PT    "/" DISK_DRIVE_NAME ":"
#define MOUNT_POINT      DISK_MOUNT_PT 
static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type     = FS_FATFS,
    .fs_data  = &fat_fs,
    .mnt_point= NULL, 
};
#else
#error "Enable CONFIG_FAT_FILESYSTEM_ELM for SD card support"
#endif

LOG_MODULE_REGISTER(wav_player_app, CONFIG_LOG_DEFAULT_LEVEL);

#define I2S_SAMPLES_PER_BLOCK   64 


#define MAX_I2S_CHANNELS        2
#define MAX_I2S_SAMPLE_BYTES    2 // 2Bytes per sample value
#define I2S_MAX_BLOCK_SIZE_BYTES (I2S_SAMPLES_PER_BLOCK * MAX_I2S_CHANNELS * MAX_I2S_SAMPLE_BYTES) //  256 bytes

#define NUM_I2S_BLOCKS 20 

#ifdef CONFIG_NOCACHE_MEMORY
    #define MEM_SLAB_CACHE_ATTR __nocache
#else
    #define MEM_SLAB_CACHE_ATTR
#endif 

static char MEM_SLAB_CACHE_ATTR __aligned(WB_UP(32))
    _k_mem_slab_buf_tx_0_mem_slab[(NUM_I2S_BLOCKS) * WB_UP(I2S_MAX_BLOCK_SIZE_BYTES)];

static STRUCT_SECTION_ITERABLE(k_mem_slab, tx_0_mem_slab) =
    Z_MEM_SLAB_INITIALIZER(tx_0_mem_slab, _k_mem_slab_buf_tx_0_mem_slab,
                           WB_UP(I2S_MAX_BLOCK_SIZE_BYTES), NUM_I2S_BLOCKS);

#define WAV_FILENAME "test.wav" 

typedef struct __attribute__((packed)) {    // make sure the WAV file is in the same format as the struct, for converting file to WAV follow this website https://www.freeconvert.com/mp3-to-wav.
    char     RIFF[4];        
    uint32_t chunkSize;      
    char     WAVE[4];        
    char     fmt_[4];        
    uint32_t fmtChunkSize;   
    uint16_t audioFormat;    
    uint16_t numChannels;    
    uint32_t sampleRate;     
    uint32_t byteRate;       
    uint16_t blockAlign;     
    uint16_t bitsPerSample;  
} WavHeaderBase;

typedef struct __attribute__((packed)) {
    char     chunkID[4];
    uint32_t chunkSize; 
} WavChunkHeader;


static int mount_sd(const char *mount_pt_arg) 
{
    int rc;
    static bool inited;

    if (!inited) {
        rc = disk_access_init(DISK_DRIVE_NAME);
        if (rc) {
            LOG_ERR("disk_access_init(%s) failed: %d", DISK_DRIVE_NAME, rc);
            return rc;
        }
        inited = true;
    }

    mp.mnt_point = mount_pt_arg; 
    rc = fs_mount(&mp);
    if (rc != FR_OK) {
        LOG_ERR("fs_mount(%s) error: %d (FR_OK is %d)", mount_pt_arg, rc, FR_OK);
        if (rc == FR_NO_FILESYSTEM) {
            LOG_ERR("No FAT filesystem found on SD card.");
        }
        return -EIO;
    }
    LOG_INF("Mounted %s at %s", DISK_DRIVE_NAME, mount_pt_arg);
    return 0;
}


int main(void)
{
    int rc;
    struct fs_file_t wav_file;
    WavHeaderBase wav_header_base;
    WavChunkHeader data_chunk_header;

    struct i2s_config i2s_cfg;
    const struct device *dev_i2s = DEVICE_DT_GET(DT_ALIAS(test));

    uint32_t wav_sample_rate;
    uint16_t wav_bits_per_sample;
    uint16_t wav_num_channels;
    uint32_t wav_data_chunk_size;
    size_t i2s_actual_block_size_bytes;

    LOG_INF("Initalizing");

    if (!device_is_ready(dev_i2s)) {
        LOG_ERR("I2S device %s not ready", dev_i2s->name);
        return -ENODEV;
    }

    rc = mount_sd(MOUNT_POINT); 
    if (rc != 0) {
        LOG_ERR("Failed to mount SD card: %d", rc);
        return rc;
    }

    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, WAV_FILENAME); 
    filepath[sizeof(filepath)-1] = '\0';

    fs_file_t_init(&wav_file);
    rc = fs_open(&wav_file, filepath, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open %s: %d", filepath, rc);
        fs_unmount(&mp);
        return rc;
    }
    LOG_INF("WAV file: %s", filepath);

    ssize_t bytes_read = fs_read(&wav_file, &wav_header_base, sizeof(WavHeaderBase));
    if (bytes_read < sizeof(WavHeaderBase)) {
        LOG_ERR("Failed to read WAV header base: read %zd bytes, expected %zu. Error: %d",
                bytes_read, sizeof(WavHeaderBase), (bytes_read < 0) ? errno: 0);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return (bytes_read < 0) ? bytes_read : -EIO;
    }

    wav_header_base.chunkSize = sys_le32_to_cpu(wav_header_base.chunkSize);     // converting to host endian for portability
    wav_header_base.fmtChunkSize = sys_le32_to_cpu(wav_header_base.fmtChunkSize);
    wav_header_base.audioFormat = sys_le16_to_cpu(wav_header_base.audioFormat);
    wav_header_base.numChannels = sys_le16_to_cpu(wav_header_base.numChannels);
    wav_header_base.sampleRate = sys_le32_to_cpu(wav_header_base.sampleRate);
    wav_header_base.byteRate = sys_le32_to_cpu(wav_header_base.byteRate);
    wav_header_base.blockAlign = sys_le16_to_cpu(wav_header_base.blockAlign);
    wav_header_base.bitsPerSample = sys_le16_to_cpu(wav_header_base.bitsPerSample);

    if (strncmp(wav_header_base.RIFF, "RIFF", 4) != 0 ||
        strncmp(wav_header_base.WAVE, "WAVE", 4) != 0 ||
        strncmp(wav_header_base.fmt_, "fmt ", 4) != 0) {
        LOG_ERR("Invalid WAV file format (RIFF/WAVE/fmt markers missing)");
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -EINVAL;
    }
    if (wav_header_base.audioFormat != 1) {
        LOG_ERR("Unsupported WAV audio format: %u (expected 1 for PCM)", wav_header_base.audioFormat);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -ENOTSUP;
    }
    if (wav_header_base.bitsPerSample != 16 && wav_header_base.bitsPerSample != 8) {
        LOG_ERR("Unsupported bits per sample: %u (expected 8 or 16)", wav_header_base.bitsPerSample);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -ENOTSUP;
    }
     if (wav_header_base.numChannels == 0 || wav_header_base.numChannels > MAX_I2S_CHANNELS) {
        LOG_ERR("Unsupported number of channels: %u (max %d)", wav_header_base.numChannels, MAX_I2S_CHANNELS);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -ENOTSUP;
    }

    wav_sample_rate = wav_header_base.sampleRate;
    wav_bits_per_sample = wav_header_base.bitsPerSample;
    wav_num_channels = wav_header_base.numChannels;

    LOG_INF("WAV Info: %"PRIu32" Hz, %"PRIu16"-bit, %"PRIu16" channels",
            wav_sample_rate, wav_bits_per_sample, wav_num_channels);

    if (wav_header_base.fmtChunkSize > 16) { 
        rc = fs_seek(&wav_file, wav_header_base.fmtChunkSize - 16, FS_SEEK_CUR);
        if (rc < 0) {
            LOG_ERR("Failed to seek past extra fmt data: %d", rc);
            fs_close(&wav_file);
            fs_unmount(&mp);
            return rc;
        }
    }

    bool data_chunk_found = false;
    while (true) {
        bytes_read = fs_read(&wav_file, &data_chunk_header, sizeof(WavChunkHeader));
        if (bytes_read < sizeof(WavChunkHeader)) {
            LOG_ERR("Failed to read data chunk header: %zd", bytes_read);
            fs_close(&wav_file);
            fs_unmount(&mp);
            return (bytes_read < 0) ? bytes_read : -EIO;
        }
        data_chunk_header.chunkSize = sys_le32_to_cpu(data_chunk_header.chunkSize);

        if (strncmp(data_chunk_header.chunkID, "data", 4) == 0) {
            wav_data_chunk_size = data_chunk_header.chunkSize;
            data_chunk_found = true;
            LOG_INF("Found 'data' chunk, size: %"PRIu32" bytes", wav_data_chunk_size);
            break;
        } else {
            LOG_INF("Skipping chunk '%.4s', size %"PRIu32, data_chunk_header.chunkID, data_chunk_header.chunkSize);
            rc = fs_seek(&wav_file, data_chunk_header.chunkSize, FS_SEEK_CUR);
            if (rc < 0) {
                LOG_ERR("Failed to seek past chunk '%.4s': %d", data_chunk_header.chunkID, rc);
                fs_close(&wav_file);
                fs_unmount(&mp);
                return rc;
            }
        }
        if (fs_tell(&wav_file) >= (wav_header_base.chunkSize + 8)) {
            LOG_ERR("Reached end of file or malformed WAV while searching for 'data' chunk.");
            break;
        }
    }

    if (!data_chunk_found) {
        LOG_ERR("'data' chunk not found in WAV file.");
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -EINVAL;
    }
    if (wav_data_chunk_size == 0) {
        LOG_ERR("WAV 'data' chunk is empty.");
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -EINVAL;
    }

    i2s_cfg.word_size = wav_bits_per_sample;
    i2s_cfg.channels = wav_num_channels;
    i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
    i2s_cfg.frame_clk_freq = wav_sample_rate;

    i2s_actual_block_size_bytes = I2S_SAMPLES_PER_BLOCK * wav_num_channels * (wav_bits_per_sample / 8);
    if (i2s_actual_block_size_bytes > I2S_MAX_BLOCK_SIZE_BYTES) {
        LOG_ERR("Calculated I2S block size (%"PRIuMAX") exceeds max slab block size (%"PRIuMAX")",
                (uintmax_t)i2s_actual_block_size_bytes, (uintmax_t)I2S_MAX_BLOCK_SIZE_BYTES);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return -EINVAL;
    }
    i2s_cfg.block_size = i2s_actual_block_size_bytes;

    i2s_cfg.timeout = 2000;
    i2s_cfg.options = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER;
    i2s_cfg.mem_slab = &tx_0_mem_slab;

    rc = i2s_configure(dev_i2s, I2S_DIR_TX, &i2s_cfg);
    if (rc < 0) {
        LOG_ERR("Failed to configure I2S stream: %d", rc);
        fs_close(&wav_file);
        fs_unmount(&mp);
        return rc;
    }
    LOG_INF("I2S configured: %"PRIu32" Hz, %u-bit, %u channels, block_size %zu bytes",
            i2s_cfg.frame_clk_freq, i2s_cfg.word_size, i2s_cfg.channels, i2s_cfg.block_size);

    void *tx_mem_block;
    uint32_t total_bytes_played = 0;
    bool i2s_started = false;

    LOG_INF("Playback");

    while (total_bytes_played < wav_data_chunk_size) {
        rc = k_mem_slab_alloc(&tx_0_mem_slab, &tx_mem_block, K_SECONDS(1));
        if (rc < 0) {
            LOG_ERR("Failed to allocate I2S TX memory block: %d. Playback may be starved.", rc);
            break;
        }

        size_t bytes_to_read_this_chunk = MIN(i2s_cfg.block_size, wav_data_chunk_size - total_bytes_played);
        bytes_read = fs_read(&wav_file, tx_mem_block, bytes_to_read_this_chunk);

        if (bytes_read < 0) {
            LOG_ERR("Failed to read WAV data: %d (errno %d)", (int)bytes_read, errno);
            k_mem_slab_free(&tx_0_mem_slab, tx_mem_block);
            break;
        }
        if (bytes_read == 0 && bytes_to_read_this_chunk > 0) { 
            LOG_INF("End of WAV data chunk reached unexpectedly or file truncated.");
            k_mem_slab_free(&tx_0_mem_slab, tx_mem_block);
            break;
        }
        if (bytes_read == 0 && bytes_to_read_this_chunk == 0) { 
             k_mem_slab_free(&tx_0_mem_slab, tx_mem_block); 
             break;
        }


        if (bytes_read < i2s_cfg.block_size) {
            memset((uint8_t *)tx_mem_block + bytes_read, 0, i2s_cfg.block_size - bytes_read);
        }

        rc = i2s_write(dev_i2s, tx_mem_block, i2s_cfg.block_size);
        if (rc < 0) {
            LOG_ERR("I2S write error: %d. Freeing block.", rc);
            k_mem_slab_free(&tx_0_mem_slab, tx_mem_block);
            break;
        }

        if (!i2s_started) {
            rc = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
            if (rc < 0) {
                LOG_ERR("Failed to trigger I2S START: %d", rc);
                break;
            }
            i2s_started = true;
            LOG_INF("I2S transmission started.");
        }
        total_bytes_played += bytes_read;
    }

    if (i2s_started) {
        LOG_INF("Draining I2S TX queue");
        rc = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
        if (rc < 0) {
            LOG_ERR("Failed to DRAIN I2S TX: %d", rc);
        } else {
            LOG_INF("I2S DRAIN complete.");
        }
    } else if (total_bytes_played > 0) { 
        LOG_WRN("I2S START trigger failed");
         rc = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN); 
        if (rc < 0) LOG_ERR("Post-failure DRAIN I2S TX also failed: %d", rc);
    }
     else {
         LOG_INF("I2S was not started or no data written");
    }

    fs_close(&wav_file);
    LOG_INF("Closed WAV file.");

    while (1) {
        k_sleep(K_SECONDS(5));
    }
    return 0;
}