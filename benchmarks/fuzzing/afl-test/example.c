#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "jpeglib.h"
#include <setjmp.h>

struct my_error_mgr {
  struct jpeg_error_mgr pub;    /* "public" fields */
  jmp_buf setjmp_buffer;        /* for return to caller */
};

typedef struct my_error_mgr * my_error_ptr;

// Longjmp out on errors
METHODDEF(void)
my_error_exit(j_common_ptr cinfo)
{
  my_error_ptr myerr = (my_error_ptr) cinfo->err;
  longjmp(myerr->setjmp_buffer, 1);
}


// Eat warnings
METHODDEF(void)
emit_message(j_common_ptr cinfo, int msg_level) {}

GLOBAL(int)
read_JPEG_file (char * filename, unsigned char *filebuf, size_t filebuflen)
{
  struct jpeg_decompress_struct cinfo;
  struct my_error_mgr jerr;
  JSAMPARRAY buffer;            /* Output row buffer */
  int row_stride;               /* physical row width in output buffer */
  int fd;
  ssize_t flen;

  fd = open(filename, O_RDONLY);
  if(fd == -1){
    return 0;
  }

  flen = read(fd, (void*)filebuf, filebuflen);
  close(fd);

  if(flen <= 0){
    return 0;
  }

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = my_error_exit;
  jerr.pub.emit_message = emit_message;

  /* Establish the setjmp return context for my_error_exit to use. */
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_decompress(&cinfo);
    return 0;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, filebuf, flen);
  (void) jpeg_read_header(&cinfo, TRUE);
  (void) jpeg_start_decompress(&cinfo);
  row_stride = cinfo.output_width * cinfo.output_components;
  buffer = (*cinfo.mem->alloc_sarray)
                ((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);

  while (cinfo.output_scanline < cinfo.output_height) {
    (void) jpeg_read_scanlines(&cinfo, buffer, 1);
  }

  (void) jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return 1;
}

int main(int argc, char *argv[]) {
  void *filebuf = NULL;
  const size_t filebuflen = 32 * 1024;

  if(argc != 2) { fprintf(stderr, "Nice usage noob\n"); return -1; }

  filebuf = malloc(filebuflen);
  if(!filebuf) {
    return -1;
  }

  while(__AFL_LOOP(100000)) {
    read_JPEG_file(argv[1], filebuf, filebuflen);
  }
}
