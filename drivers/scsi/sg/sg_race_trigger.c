/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Changhui Zhong <czhong@redhat.com> All rights reserved. */

/*
 * sg_race_trigger.c - Reproduce WRITE(6) bogus elapsed time bug
 * 
 * Bug: /proc/scsi/sg/debug shows bogus elapsed time due to race condition
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <pthread.h>
#include <signal.h>

#define PROC_DEBUG "/proc/scsi/sg/debug"
#define NUM_THREADS 8
#define TEST_ITERATIONS 1000

char device_path[64] = {0};  /* Dynamically determined sg device */
volatile int keep_running = 1;
int bugs_found = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

#define BUG_LOG_FILE "bug_find.log"

/* SCSI commands */
unsigned char test_unit_ready[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
unsigned char read6[6]  = {0x08, 0x00, 0x00, 0x00, 0x01, 0x00};
unsigned char write6[6] = {0x0a, 0x00, 0x00, 0x00, 0x01, 0x00};

void cleanup(int sig) {
    keep_running = 0;
}

/* Find the sg device for scsi_debug */
int find_scsi_debug_sg() {
    FILE *fp;
    char line[256];
    char model_path[256];
    char model[64];
    int sg_num;
    
    /* Method 1: Try lsscsi first (most reliable if available) */
    fp = popen("lsscsi 2>/dev/null | grep scsi_debug | awk '{print $NF}'", "r");
    if (fp && fgets(line, sizeof(line), fp)) {
        if (strstr(line, "/dev/sg")) {
            sscanf(line, "%s", device_path);
            pclose(fp);
            if (access(device_path, F_OK) == 0) {
                return 0;
            }
        }
    }
    if (fp) pclose(fp);
    
    /* Method 2: Check /sys/class/scsi_generic/ */
    for (sg_num = 0; sg_num < 32; sg_num++) {
        snprintf(model_path, sizeof(model_path), 
                 "/sys/class/scsi_generic/sg%d/device/model", sg_num);
        
        fp = fopen(model_path, "r");
        if (!fp) continue;
        
        if (fgets(model, sizeof(model), fp)) {
            /* Remove trailing whitespace */
            char *p = model + strlen(model) - 1;
            while (p >= model && (*p == '\n' || *p == ' ' || *p == '\r')) {
                *p-- = '\0';
            }
            
            if (strstr(model, "scsi_debug")) {
                snprintf(device_path, sizeof(device_path), "/dev/sg%d", sg_num);
                fclose(fp);
                
                /* Verify device exists */
                if (access(device_path, F_OK) == 0) {
                    return 0;
                }
            }
        }
        fclose(fp);
    }
    
    /* Method 3: Check /proc/scsi/scsi */
    fp = popen("grep -A3 scsi_debug /proc/scsi/scsi 2>/dev/null | grep 'Host:' | awk '{print $2, $4, $6, $8}' | tr -d ','", "r");
    if (fp && fgets(line, sizeof(line), fp)) {
        int host, channel, id, lun;
        if (sscanf(line, "scsi%d Channel: %d Id: %d Lun: %d", &host, &channel, &id, &lun) == 4) {
            /* Try to find corresponding sg device */
            for (sg_num = 0; sg_num < 32; sg_num++) {
                char link_path[256], target[256];
                snprintf(link_path, sizeof(link_path), "/sys/class/scsi_generic/sg%d", sg_num);
                
                ssize_t len = readlink(link_path, target, sizeof(target) - 1);
                if (len > 0) {
                    target[len] = '\0';
                    char expected[64];
                    snprintf(expected, sizeof(expected), "%d:%d:%d:%d", host, channel, id, lun);
                    if (strstr(target, expected)) {
                        snprintf(device_path, sizeof(device_path), "/dev/sg%d", sg_num);
                        pclose(fp);
                        if (access(device_path, F_OK) == 0) {
                            return 0;
                        }
                    }
                }
            }
        }
    }
    if (fp) pclose(fp);
    
    fprintf(stderr, "Error: Could not find sg device for scsi_debug\n");
    fprintf(stderr, "Tried: lsscsi, /sys/class/scsi_generic/, /proc/scsi/scsi\n");
    return -1;
}

/* Setup scsi_debug device */
void setup_device() {
    printf("Setting up scsi_debug tape device...\n");
    system("rmmod scsi_debug 2>/dev/null");
    system("modprobe scsi_debug ptype=1 delay=1000 ndelay=500000 max_luns=1 num_tgts=1");
    sleep(2);
    
    /* Find the sg device */
    if (find_scsi_debug_sg() != 0) {
        fprintf(stderr, "Failed to find scsi_debug sg device\n");
        exit(1);
    }
    
    /* Ensure device has proper permissions */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "chmod 666 %s 2>/dev/null", device_path);
    system(cmd);
    
    printf("✓ Device ready: %s\n\n", device_path);
}

/* Cleanup scsi_debug device */
void cleanup_device() {
    printf("\nCleaning up scsi_debug device...\n");
    system("rmmod scsi_debug 2>/dev/null");
    printf("✓ Cleanup complete\n");
}

/* Send SCSI command via SG_IO */
void send_scsi_cmd(int fd, unsigned char *cmd, int dxfer_direction) {
    sg_io_hdr_t io;
    unsigned char sense[32];
    unsigned char buffer[512];
    
    memset(&io, 0, sizeof(io));
    io.interface_id = 'S';
    io.cmd_len = 6;
    io.cmdp = cmd;
    io.sbp = sense;
    io.mx_sb_len = sizeof(sense);
    io.dxfer_direction = dxfer_direction;
    io.dxfer_len = (dxfer_direction == SG_DXFER_NONE) ? 0 : 512;
    io.dxferp = buffer;
    io.timeout = 60000;
    
    ioctl(fd, SG_IO, &io);
}

/* I/O worker thread - continuously send SCSI commands */
void *io_worker(void *arg) {
    int fd = open(device_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open device");
        return NULL;
    }
    
    int cmd_idx = 0;
    while (keep_running) {
        switch (cmd_idx++ % 3) {
            case 0: send_scsi_cmd(fd, test_unit_ready, SG_DXFER_NONE); break;
            case 1: send_scsi_cmd(fd, read6, SG_DXFER_FROM_DEV); break;
            case 2: send_scsi_cmd(fd, write6, SG_DXFER_TO_DEV); break;
        }
        usleep(100);
    }
    
    close(fd);
    return NULL;
}

/* Parse elapsed time from debug line */
long parse_elapsed(const char *line) {
    char *p = strstr(line, "t_o/elap=");
    if (p) {
        /* Format: t_o/elap=timeout/elapsedms */
        int timeout, elapsed;
        if (sscanf(p, "t_o/elap=%d/%dms", &timeout, &elapsed) == 2) {
            return elapsed;
        }
    }
    return -1;
}

/* Get opcode from debug line */
void get_opcode(const char *line, char *opcode) {
    char *p = strstr(line, "op=0x");
    if (p) {
        sscanf(p, "op=0x%s", opcode);
        // Remove trailing characters
        for (int i = 0; i < strlen(opcode); i++) {
            if (opcode[i] < '0' || (opcode[i] > '9' && opcode[i] < 'a') || opcode[i] > 'f') {
                opcode[i] = '\0';
                break;
            }
        }
    } else {
        strcpy(opcode, "??");
    }
}

/* Log complete /proc/scsi/sg/debug content when bug is found */
void log_debug_snapshot(int iteration, long elapsed, const char *opcode, const char *debug_content) {
    FILE *log_fp;
    time_t now;
    struct tm *tm_info;
    char time_str[64];
    
    /* Open log file in append mode */
    log_fp = fopen(BUG_LOG_FILE, "a");
    if (!log_fp) {
        fprintf(stderr, "Warning: Failed to open %s for writing\n", BUG_LOG_FILE);
        return;
    }
    
    /* Get current time */
    time(&now);
    tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    /* Write header */
    fprintf(log_fp, "\n");
    fprintf(log_fp, "================================================================================\n");
    fprintf(log_fp, "BUG DETECTED at %s\n", time_str);
    fprintf(log_fp, "================================================================================\n");
    fprintf(log_fp, "Iteration:     %d\n", iteration);
    fprintf(log_fp, "Elapsed time:  %ld ms\n", elapsed);
    fprintf(log_fp, "Opcode:        0x%s\n", opcode);
    fprintf(log_fp, "--------------------------------------------------------------------------------\n");
    fprintf(log_fp, "Complete /proc/scsi/sg/debug snapshot:\n");
    fprintf(log_fp, "--------------------------------------------------------------------------------\n");
    
    /* Write the saved debug content */
    fputs(debug_content, log_fp);
    
    fprintf(log_fp, "================================================================================\n");
    fprintf(log_fp, "\n");
    
    fclose(log_fp);
}

/* Monitor thread - check /proc/scsi/sg/debug for bogus values */
void *monitor_worker(void *arg) {
    FILE *fp;
    char line[512];
    int iteration = 0;
    
    printf("Starting monitor (will run %d iterations)...\n\n", TEST_ITERATIONS);
    
    while (keep_running && iteration < TEST_ITERATIONS) {
        /* Read entire debug output into a buffer first */
        char debug_buffer[65536] = {0};
        int buf_pos = 0;
        
        fp = fopen(PROC_DEBUG, "r");
        if (!fp) {
            perror("fopen");
            sleep(1);
            continue;
        }
        
        /* Save complete debug output */
        while (fgets(line, sizeof(line), fp) && buf_pos < sizeof(debug_buffer) - 512) {
            int len = strlen(line);
            memcpy(debug_buffer + buf_pos, line, len);
            buf_pos += len;
        }
        fclose(fp);
        
        /* Now check for bugs in the saved buffer */
        char *line_ptr = debug_buffer;
        char *next_line;
        char saved_line[512];
        int found_bug = 0;
        long bug_elapsed = 0;
        char bug_opcode[8] = {0};
        
        while (*line_ptr && (next_line = strchr(line_ptr, '\n'))) {
            size_t line_len = next_line - line_ptr;
            if (line_len < sizeof(saved_line)) {
                memcpy(saved_line, line_ptr, line_len);
                saved_line[line_len] = '\0';
                
                if (strstr(saved_line, "elap=")) {
                    long elapsed = parse_elapsed(saved_line);
                    
                    /* Detect bogus: negative, very large, or suspiciously round numbers */
                    if (elapsed < 0 || elapsed > 10000) {
                        char opcode[8];
                        get_opcode(saved_line, opcode);
                        
                        /* Save first bug info for logging */
                        if (!found_bug) {
                            found_bug = 1;
                            bug_elapsed = elapsed;
                            strcpy(bug_opcode, opcode);
                        }
                        
                        pthread_mutex_lock(&mutex);
                        bugs_found++;
                        
                        /* Log complete debug snapshot to file */
                        log_debug_snapshot(iteration, elapsed, opcode, debug_buffer);
                        
                        printf("\n");
                        printf("════════════════════════════════════════════════════════════════\n");
                        printf("BOGUS ELAPSED TIME DETECTED!\n");
                        printf("════════════════════════════════════════════════════════════════\n");
                        printf("Iteration:     %d\n", iteration);
                        printf("Elapsed time:  %ld ms\n", elapsed);
                        printf("Opcode:        0x%s", opcode);
                        
                        if (strcmp(opcode, "0a") == 0) {
                            printf(" ← WRITE(6) ★★★ This is the bug from original report!\n");
                        } else if (strcmp(opcode, "08") == 0) {
                            printf(" ← READ(6)\n");
                        } else if (strcmp(opcode, "00") == 0) {
                            printf(" ← TEST UNIT READY\n");
                        } else {
                            printf("\n");
                        }
                        
                        printf("Debug line:    %s\n", saved_line);
                        printf("Logged to:     %s\n", BUG_LOG_FILE);
                        printf("════════════════════════════════════════════════════════════════\n");
                        pthread_mutex_unlock(&mutex);
                    }
                }
            }
            
            line_ptr = next_line + 1;
        }
        
        iteration++;
        
        if (iteration % 100 == 0) {
            pthread_mutex_lock(&mutex);
            printf("[Progress: %d/%d iterations, %d bugs found]\n", 
                   iteration, TEST_ITERATIONS, bugs_found);
            pthread_mutex_unlock(&mutex);
        }
        
        usleep(100);
    }
    
    keep_running = 0;
    return NULL;
}

int main() {
    pthread_t io_threads[NUM_THREADS];
    pthread_t monitor_thread;
    
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    
    printf("\n");
    printf("════════════════════════════════════════════════════════════════\n");
    printf("  SCSI sg Race Condition Bug Reproducer\n");
    printf("  Target: WRITE(6) (op=0x0a) bogus elapsed time\n");
    printf("════════════════════════════════════════════════════════════════\n");
    printf("\n");
    
    /* Clear/create log file */
    FILE *log_fp = fopen(BUG_LOG_FILE, "w");
    if (log_fp) {
        fprintf(log_fp, "Bug Detection Log\n");
        fprintf(log_fp, "Started at: %s\n", __DATE__ " " __TIME__);
        fprintf(log_fp, "Log file will contain complete /proc/scsi/sg/debug snapshots when bugs are detected\n");
        fprintf(log_fp, "\n");
        fclose(log_fp);
        printf("Log file: %s (cleared)\n\n", BUG_LOG_FILE);
    } else {
        fprintf(stderr, "Warning: Could not create %s\n\n", BUG_LOG_FILE);
    }
    
    /* Setup */
    setup_device();
    
    /* Start I/O threads */
    printf("Starting %d I/O threads...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&io_threads[i], NULL, io_worker, NULL);
    }
    printf("✓ I/O threads running\n\n");
    
    /* Start monitor thread */
    pthread_create(&monitor_thread, NULL, monitor_worker, NULL);
    
    /* Wait for monitor to complete */
    pthread_join(monitor_thread, NULL);
    
    /* Stop I/O threads */
    keep_running = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(io_threads[i], NULL);
    }
    
    /* Summary */
    printf("\n");
    printf("════════════════════════════════════════════════════════════════\n");
    printf("  Test Complete!\n");
    printf("════════════════════════════════════════════════════════════════\n");
    printf("Total bugs found: %d\n", bugs_found);
    
    if (bugs_found > 0) {
        printf("\n✓ BUG SUCCESSFULLY REPRODUCED!\n");
        printf("  Found %d instances of bogus elapsed time values\n", bugs_found);
        printf("\n Complete debug logs saved to: %s\n", BUG_LOG_FILE);
        printf("  Use 'cat %s' or 'less %s' to view\n", BUG_LOG_FILE, BUG_LOG_FILE);
    } else {
        printf("\n✗ No bugs detected in this run\n");
        printf("  Try running again (race conditions are timing-dependent)\n");
    }
    printf("════════════════════════════════════════════════════════════════\n");
    
    /* Cleanup */
    cleanup_device();
    
    return (bugs_found > 0) ? 0 : 1;
}

