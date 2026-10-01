#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/sidechannel.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jbig/jbig.h"
#include "xqx.h"

#define be32(x)                                                                \
  ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) |                    \
   (((x) & 0x0000ff00) << 8) | (((x) & 0x000000ff) << 24))
#define be16(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))

static int EconoMode = 0;
static int PrintDensity = 3;
static int SourceCode = DMBIN_AUTO;
static int MediaCode = DMMEDIA_PLAIN;
static int Duplex = DMDUPLEX_OFF;
static int PageNum = 0;
static int TotalCopies = 1;

static volatile int JobFinished = 0;
static volatile int HasErrorState = 0;

static long JbgOptions[5] = {
    /* Order */
    JBG_ILEAVE | JBG_SMID,
    /* Options */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON,
    /* L0 */
    128,
    /* MX */
    16,
    /* MY */
    0};

typedef struct _BIE_CHAIN {
  unsigned char *data;
  size_t len;
  struct _BIE_CHAIN *next;
} BIE_CHAIN;

static void chunk_write(unsigned long type, unsigned long items, FILE *fp) {
  XQX_HEADER chunk;
  chunk.type = be32(type);
  chunk.items = be32(items);
  fwrite(&chunk, 1, sizeof(XQX_HEADER), fp);
}

static void item_uint32_write(unsigned long item, unsigned long value,
                              FILE *fp) {
  XQX_ITEM_UINT32 item_uint32;
  item_uint32.header.type = be32(item);
  item_uint32.header.size = be32(sizeof(DWORD));
  item_uint32.value = be32(value);
  fwrite(&item_uint32, 1, sizeof(XQX_ITEM_UINT32), fp);
}

static void item_bytelut_write(unsigned long item, unsigned long len, BYTE *buf,
                               FILE *fp) {
  XQX_ITEM_HEADER header;
  header.type = be32(item);
  header.size = be32(len);
  fwrite(&header, 1, sizeof(XQX_ITEM_HEADER), fp);
  fwrite(buf, 1, len, fp);
}

static void free_chain(BIE_CHAIN *chain) {
  BIE_CHAIN *next;
  while (chain) {
    next = chain->next;
    if (chain->data)
      free(chain->data);
    free(chain);
    chain = next;
  }
}

static void output_jbig(unsigned char *start, size_t len, void *cbarg) {
  BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
  int size = 65536;

  if (*root == NULL) {
    *root = malloc(sizeof(BIE_CHAIN));
    if (!*root)
      return;
    (*root)->data = NULL;
    (*root)->next = NULL;
    (*root)->len = 0;
    size = 20;
  }

  current = *root;
  while (current->next)
    current = current->next;

  while (len > 0) {
    if (!current->data) {
      current->data = malloc(size);
      if (!current->data)
        return;
    }

    int left = size - current->len;
    int amt = (len > (size_t)left) ? left : (int)len;
    memcpy(current->data + current->len, start, amt);
    current->len += amt;
    len -= amt;
    start += amt;

    if (current->len == (size_t)size) {
      current->next = malloc(sizeof(BIE_CHAIN));
      if (!current->next)
        return;
      current = current->next;
      current->data = NULL;
      current->next = NULL;
      current->len = 0;
    }
  }
}

static int IsOnline = 1;

static void parse_pjl_status_line(const char *line) {
  if (!line || !line[0])
    return;
  fprintf(stderr, "DEBUG: Backchannel PJL Status: [%s]\n", line);

  if (strstr(line, "ONLINE=FALSE")) {
    IsOnline = 0;
  } else if (strstr(line, "ONLINE=TRUE")) {
    IsOnline = 1;
    if (HasErrorState) {
      fprintf(stderr, "STATE: -door-open-warning -media-empty-warning "
                      "-media-jam-warning -marker-supply-empty-warning\n");
      fprintf(stderr, "INFO: Printer ready. Printing resumed.\n");
      fflush(stderr);
      HasErrorState = 0;
    }
  }

  // Check for Paper Jam
  if (strstr(line, "CODE=10003") || strstr(line, "CODE=42000") ||
      strstr(line, "CODE=41001") || strstr(line, "PAPER JAM") ||
      strstr(line, "JAM IN TRAY") || strstr(line, "JAM IN CARTRIDGE") ||
      strstr(line, "JAM")) {
    fprintf(
        stderr,
        "STATE: +media-jam-warning -door-open-warning -media-empty-warning\n");
    fprintf(stderr,
            "INFO: Paper jam detected (E0). Please clear paper path.\n");
    fflush(stderr);
    HasErrorState = 1;
  }
  // Check for Out of Paper
  else if (strstr(line, "CODE=41004") || strstr(line, "OUT OF PAPER") ||
           strstr(line, "LOAD PAPER") || strstr(line, "PAPER OUT") ||
           strstr(line, "MEDIA EMPTY") || strstr(line, "NO PAPER") ||
           strstr(line, "TRAY EMPTY") || strstr(line, "INSERT PAPER")) {
    fprintf(
        stderr,
        "STATE: +media-empty-warning -door-open-warning -media-jam-warning\n");
    fprintf(stderr, "INFO: Paper tray is empty (E1). Please load paper.\n");
    fflush(stderr);
    HasErrorState = 1;
  }
  // Check for Door / Cartridge Cover Open
  else if (strstr(line, "DOOR OPEN") || strstr(line, "COVER OPEN") ||
           strstr(line, "TOP COVER OPEN") || strstr(line, "CODE=41003") ||
           (!IsOnline && strstr(line, "CODE=10023"))) {
    fprintf(
        stderr,
        "STATE: +door-open-warning -media-empty-warning -media-jam-warning\n");
    fprintf(stderr,
            "INFO: Printer cover / door is open (E2). Please close cover.\n");
    fflush(stderr);
    HasErrorState = 1;
  }
  // Check for Cartridge Missing / Not Seated
  else if (strstr(line, "CODE=10004") || strstr(line, "INSTALL CARTRIDGE") ||
           strstr(line, "NO TONER")) {
    fprintf(stderr, "STATE: +marker-supply-empty-warning\n");
    fprintf(stderr, "INFO: Toner cartridge missing or unseated (E3).\n");
    fflush(stderr);
    HasErrorState = 1;
  }
  // Check for Ready / Normal Idle
  else if (strstr(line, "ONLINE=TRUE") || strstr(line, "READY") ||
           strstr(line, "CODE=10001")) {
    if (HasErrorState) {
      fprintf(stderr, "STATE: -door-open-warning -media-empty-warning "
                      "-media-jam-warning -marker-supply-empty-warning\n");
      fprintf(stderr, "INFO: Printer ready.\n");
      fflush(stderr);
      HasErrorState = 0;
    }
  }
}

static void *status_monitor_thread(void *arg) {
  char buf[512];
  char line[512];
  int line_pos = 0;
  int idle_count = 0;

  // Monitor while job is sending for errors and up to 15 seconds after job is
  // done
  while (!JobFinished || HasErrorState || idle_count < 15) {
    ssize_t bytes = cupsBackChannelRead(buf, sizeof(buf) - 1, 1.0);
    if (bytes > 0) {
      buf[bytes] = '\0';
      idle_count = 0;
      for (ssize_t i = 0; i < bytes; i++) {
        char c = buf[i];
        if (c == '\r' || c == '\n') {
          if (line_pos > 0) {
            line[line_pos] = '\0';
            parse_pjl_status_line(line);
            line_pos = 0;
          }
        } else if (line_pos < (int)(sizeof(line) - 1)) {
          line[line_pos++] = c;
        }
      }
    } else {
      if (JobFinished) {
        idle_count++;
      }
    }
  }

  if (HasErrorState) {
    fprintf(
        stderr,
        "STATE: -media-empty-warning -media-jam-warning -door-open-warning\n");
    fflush(stderr);
  }
  return NULL;
}

static void start_doc(FILE *fp) {
  char header[4] = ",XQX";
  time_t now = time(NULL);
  struct tm *tmp = localtime(&now);
  char datetime[16];
  strftime(datetime, sizeof(datetime), "%Y%m%d%H%M%S", tmp);

  fprintf(fp, "\033%%-12345X@PJL JOB\n");
  fprintf(fp, "@PJL SET JAMRECOVERY=OFF\n");
  fprintf(fp, "@PJL SET DENSITY=%d\n", PrintDensity);
  fprintf(fp, "@PJL SET ECONOMODE=%s\n", EconoMode ? "ON" : "OFF");
  fprintf(fp, "@PJL SET RET=MEDIUM\n");
  fprintf(fp, "@PJL INFO STATUS\n");
  fprintf(fp, "@PJL USTATUS DEVICE = ON\n");
  fprintf(fp, "@PJL USTATUS JOB = ON\n");
  fprintf(fp, "@PJL USTATUS PAGE = ON\n");
  fprintf(fp, "@PJL USTATUS TIMED = 5\n");
  fprintf(fp, "@PJL SET JOBATTR=\"JobAttr4=%s\"", datetime);
  fputc(0, fp);
  fprintf(fp, "\033%%-12345X");
  fwrite(header, 1, sizeof(header), fp);

  int nitems = 7;
  chunk_write(XQX_START_DOC, nitems, fp);
  item_uint32_write(0x80000000, 84, fp);
  item_uint32_write(0x10000005, 1, fp);
  item_uint32_write(0x10000001, 0, fp);
  item_uint32_write(XQXI_DMDUPLEX, (Duplex != DMDUPLEX_OFF) ? 2 : 0, fp);
  item_uint32_write(0x10000000, 0, fp);
  item_uint32_write(0x10000003, 1, fp);
  item_uint32_write(XQXI_END, 0xdeadbeef, fp);
  fflush(fp);
}

static void end_doc(FILE *fp) {
  chunk_write(XQX_END_DOC, 0, fp);
  fprintf(fp, "\033%%-12345X@PJL EOJ\n");
  fprintf(fp, "\033%%-12345X");
  fflush(fp);
}

static void start_page(BIE_CHAIN **root, int resX, int resY, int paperCode,
                       int realWidth, FILE *ofp) {
  BIE_CHAIN *current = *root;
  if (!current || !current->next || current->len != 20) {
    fprintf(stderr, "ERROR: Invalid JBIG header\n");
    return;
  }

  unsigned long w =
      (((long)current->data[4] << 24) | ((long)current->data[5] << 16) |
       ((long)current->data[6] << 8) | (long)current->data[7]);
  unsigned long h =
      (((long)current->data[8] << 24) | ((long)current->data[9] << 16) |
       ((long)current->data[10] << 8) | (long)current->data[11]);

  int nitems = (Duplex != DMDUPLEX_OFF) ? 16 : 15;
  int pause = (PageNum == 1) ? 0 : 1;

  chunk_write(XQX_START_PAGE, nitems, ofp);
  item_uint32_write(0x80000000, (Duplex == DMDUPLEX_OFF) ? 180 : 192, ofp);
  item_uint32_write(0x20000005, 1, ofp);
  item_uint32_write(XQXI_DMDEFAULTSOURCE, SourceCode, ofp);
  item_uint32_write(XQXI_DMMEDIATYPE, MediaCode, ofp);
  item_uint32_write(0x20000007, 1, ofp);

  item_uint32_write(XQXI_RESOLUTION_X, resX, ofp);
  item_uint32_write(XQXI_RESOLUTION_Y, resY, ofp);
  item_uint32_write(XQXI_RASTER_X, w, ofp);
  item_uint32_write(XQXI_RASTER_Y, h, ofp);
  item_uint32_write(XQXI_VIDEO_BPP, 1, ofp);

  item_uint32_write(XQXI_VIDEO_X, realWidth, ofp);
  item_uint32_write(XQXI_VIDEO_Y, h, ofp);
  item_uint32_write(XQXI_ECONOMODE, EconoMode, ofp);
  if (Duplex != DMDUPLEX_OFF) {
    item_uint32_write(XQXI_DUPLEX_PAUSE, pause, ofp);
  }
  item_uint32_write(XQXI_DMPAPER, paperCode, ofp);
  item_uint32_write(XQXI_END, 0xdeadbeef, ofp);

  fprintf(stderr, "PAGE: %d %d\n", PageNum, TotalCopies);
  fflush(ofp);
}

static void end_page(FILE *ofp) {
  chunk_write(XQX_END_PAGE, 0, ofp);
  fflush(ofp);
}

static void write_plane(BIE_CHAIN **root, FILE *fp) {
  BIE_CHAIN *current = *root;
  if (!current || !current->next || current->len != 20)
    return;

  BYTE *bih = current->data;
  int first = 1;
  for (current = *root; current && current->len; current = current->next) {
    if (current == *root)
      continue;
    int len = current->len;

    chunk_write(XQX_START_PLANE, 4, fp);
    item_uint32_write(0x80000000, first ? 64 : 48, fp);
    item_uint32_write(0x40000000, 0, fp);
    if (first)
      item_bytelut_write(XQXI_BIH, 20, bih, fp);
    else
      item_uint32_write(0x40000003, 1, fp);
    item_uint32_write(XQXI_END, 0xdeadbeef, fp);

    chunk_write(XQX_JBIG, len, fp);
    if (len)
      fwrite(current->data, 1, len, fp);
    chunk_write(XQX_END_PLANE, 0, fp);
    first = 0;
  }
  free_chain(*root);
  fflush(fp);
}

static int map_paper_code(const cups_page_header2_t *h) {
  if (h->cupsInteger[0] > 0)
    return (int)h->cupsInteger[0];

  const char *name = h->cupsPageSizeName;
  if (strcasecmp(name, "Letter") == 0)
    return DMPAPER_LETTER;
  if (strcasecmp(name, "A4") == 0)
    return DMPAPER_A4;
  if (strcasecmp(name, "Legal") == 0)
    return DMPAPER_LEGAL;
  if (strcasecmp(name, "Executive") == 0)
    return DMPAPER_EXECUTIVE;
  if (strcasecmp(name, "A5") == 0)
    return DMPAPER_A5;
  if (strcasecmp(name, "B5") == 0 || strcasecmp(name, "JB5") == 0)
    return DMPAPER_B5;
  if (strcasecmp(name, "Env10") == 0 || strcasecmp(name, "COM10") == 0)
    return DMPAPER_ENV_10;
  if (strcasecmp(name, "EnvDL") == 0 || strcasecmp(name, "DL") == 0)
    return DMPAPER_ENV_DL;
  if (strcasecmp(name, "EnvC5") == 0 || strcasecmp(name, "C5") == 0)
    return DMPAPER_ENV_C5;
  if (strcasecmp(name, "EnvMonarch") == 0 || strcasecmp(name, "Monarch") == 0)
    return DMPAPER_ENV_MONARCH;
  if (strcasecmp(name, "Postcard") == 0)
    return 71;
  if (strcasecmp(name, "DoublePostcard") == 0)
    return 72;
  if (strcasecmp(name, "16k195x270") == 0)
    return 257;
  if (strcasecmp(name, "16k184x260") == 0)
    return 263;
  if (strcasecmp(name, "16k195x270") == 0)
    return 264;

  if (h->PageSize[0] == 612 && h->PageSize[1] == 792)
    return DMPAPER_LETTER;
  if (h->PageSize[0] == 595 && h->PageSize[1] == 842)
    return DMPAPER_A4;
  if (h->PageSize[0] == 612 && h->PageSize[1] == 1008)
    return DMPAPER_LEGAL;

  return DMPAPER_LETTER;
}

int main(int argc, char *argv[]) {
  if (argc < 6 || argc > 7) {
    fprintf(
        stderr,
        "ERROR: Usage: rastertoxqx job-id user title copies options [file]\n");
    return 1;
  }

  TotalCopies = atoi(argv[4]);
  if (TotalCopies < 1)
    TotalCopies = 1;

  // Parse options from argv[5]
  cups_option_t *options = NULL;
  int num_options = cupsParseOptions(argv[5], 0, &options);

  const char *val;
  if ((val = cupsGetOption("EconoMode", num_options, options)) != NULL ||
      (val = cupsGetOption("economode", num_options, options)) != NULL ||
      (val = cupsGetOption("SaveToner", num_options, options)) != NULL) {
    if (strcasecmp(val, "True") == 0 || strcasecmp(val, "ON") == 0 ||
        strcasecmp(val, "1") == 0) {
      EconoMode = 1;
    }
  }

  if ((val = cupsGetOption("PrintDensity", num_options, options)) != NULL ||
      (val = cupsGetOption("density", num_options, options)) != NULL) {
    int d = atoi(val);
    if (d >= 1 && d <= 5)
      PrintDensity = d;
  }

  if ((val = cupsGetOption("InputSlot", num_options, options)) != NULL ||
      (val = cupsGetOption("media-source", num_options, options)) != NULL) {
    if (strcasecmp(val, "Manual") == 0)
      SourceCode = DMBIN_MANUAL;
    else if (strcasecmp(val, "Tray1") == 0)
      SourceCode = DMBIN_TRAY1;
    else
      SourceCode = DMBIN_AUTO;
  }

  cupsFreeOptions(num_options, options);

  int fd = 0;
  if (argc == 7) {
    fd = open(argv[6], O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "ERROR: Unable to open print file: %s\n", argv[6]);
      return 1;
    }
  }

  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) {
    fprintf(stderr, "ERROR: Unable to open CUPS raster stream\n");
    if (fd > 0)
      close(fd);
    return 1;
  }

  // Clear any previous warning state on job start
  fprintf(
      stderr,
      "STATE: -door-open-warning -media-empty-warning -media-jam-warning\n");
  fflush(stderr);

  // Launch back channel monitor thread
  pthread_t monitor_tid;
  int thread_created =
      (pthread_create(&monitor_tid, NULL, status_monitor_thread, NULL) == 0);

  cups_page_header2_t header;
  int doc_started = 0;

  while (cupsRasterReadHeader2(ras, &header)) {
    PageNum++;
    if (!doc_started) {
      start_doc(stdout);
      doc_started = 1;
    }

    int paperCode = map_paper_code(&header);
    int resX = header.HWResolution[0] ? header.HWResolution[0] : 600;
    int resY = header.HWResolution[1] ? header.HWResolution[1] : 600;
    int rawW = header.cupsWidth;
    int rawH = header.cupsHeight;
    int rawBpl = header.cupsBytesPerLine;

    // JBIG alignment requirements
    int encW = (rawW + 127) & ~127;
    int bpl = (encW + 7) / 8;
    int bpl16 = (bpl + 15) & ~15;

    unsigned char *pageBuf = calloc(bpl16, rawH);
    if (!pageBuf) {
      fprintf(stderr, "ERROR: Out of memory for page %d\n", PageNum);
      break;
    }

    unsigned char *rowBuf = malloc(rawBpl);
    if (!rowBuf) {
      free(pageBuf);
      fprintf(stderr, "ERROR: Out of memory for row buffer\n");
      break;
    }

    int invert = (header.cupsColorSpace == CUPS_CSPACE_W);

    for (int y = 0; y < rawH; y++) {
      if (cupsRasterReadPixels(ras, rowBuf, rawBpl) == 0) {
        fprintf(stderr, "ERROR: Premature end of raster data on line %d\n", y);
        break;
      }
      unsigned char *dst = pageBuf + (y * bpl16);
      int copyBytes = (rawBpl < bpl16) ? rawBpl : bpl16;
      if (invert) {
        for (int x = 0; x < copyBytes; x++) {
          dst[x] = ~rowBuf[x];
        }
      } else {
        memcpy(dst, rowBuf, copyBytes);
      }
    }
    free(rowBuf);

    // JBIG encoding
    BIE_CHAIN *chain = NULL;
    unsigned char *bitmaps[1];
    bitmaps[0] = pageBuf;
    struct jbg_enc_state se;

    jbg_enc_init(&se, encW, rawH, 1, bitmaps, output_jbig, &chain);
    jbg_enc_options(&se, JbgOptions[0], JbgOptions[1], JbgOptions[2],
                    JbgOptions[3], JbgOptions[4]);
    jbg_enc_out(&se);
    jbg_enc_free(&se);

    start_page(&chain, resX, resY, paperCode, rawW, stdout);
    write_plane(&chain, stdout);
    end_page(stdout);

    free(pageBuf);
  }

  if (doc_started) {
    end_doc(stdout);
  }

  cupsRasterClose(ras);
  if (fd > 0)
    close(fd);

  JobFinished = 1;

  if (thread_created) {
    pthread_join(monitor_tid, NULL);
  }

  fprintf(stderr, "INFO: Filter completed successfully (%d pages processed).\n",
          PageNum);
  return 0;
}
