/* readpic.c, read source pictures                                          */

/* Copyright (C) 1996, MPEG Software Simulation Group. All Rights Reserved. */

/*
 * Disclaimer of Warranty
 *
 * These software programs are available to the user without any license fee or
 * royalty on an "as is" basis.  The MPEG Software Simulation Group disclaims
 * any and all warranties, whether express, implied, or statuary, including any
 * implied warranties or merchantability or of fitness for a particular
 * purpose.  In no event shall the copyright-holder be liable for any
 * incidental, punitive, or consequential damages of any kind whatsoever
 * arising from the use of these programs.
 *
 * This disclaimer of warranty extends to the user of these programs and user's
 * customers, employees, agents, transferees, successors, and assigns.
 *
 * The MPEG Software Simulation Group does not represent or warrant that the
 * programs furnished hereunder are free of infringement of any third-party
 * patents.
 *
 * Commercial implementations of MPEG-1 and MPEG-2 video, including shareware,
 * are subject to royalty fees to patent holders.  Many of these patents are
 * general enough such that they are unavoidable regardless of implementation
 * design.
 *
 */
/* Modifications and enhancements (C) 2000/2001 Andrew Stevens */

/* These modifications are free software; you can redistribute it
 *  and/or modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

/* Modifications and enhancements 
   (C) 2000/2001 Andrew Stevens, Rainer Johanni

 */

/* These modifications are free software; you can redistribute it
 *  and/or modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include <config.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "syntaxparams.h"
#include "global.h"
#include "simd.h"
#include "mpeg2encoder.hh"
#include "mpegconsts.h"
#include "yuv4mpeg.h"
 
   /* NOTE: access toframes_read *must* be read-only in other threads
	  once the chunk-reading worker thread has been started.
   */

static pthread_mutex_t frame_buffer_lock;

static pthread_cond_t new_chunk_req = PTHREAD_COND_INITIALIZER;
static pthread_cond_t new_chunk_ack = PTHREAD_COND_INITIALIZER;
static pthread_t      worker_thread;

static volatile int frames_read = 0;
static int last_frame = -1;


/* Buffers for frame luminance means */

static int *lum_mean = NULL;

/* Buffers frame data */
static uint8_t ***frame_buffers;
int frame_buffer_size;

static int luminance_mean(uint8_t *frame )
{
	uint8_t *p;
    int stride = enc->parms.phy_width;
    int width = enc->parms.enc_width;
    int height = enc->parms.enc_height;
	int sum = 0;
    int line;
    uint8_t *line_base = frame;
    for( line = 0; line < height; ++line )
    {
        for( p = line_base; p < line_base+width ; p += 8 )
        {
            sum += (static_cast<int>(p[0]) + static_cast<int>(p[1])) 
                + (static_cast<int>(p[2]) + static_cast<int>(p[3]))
                + (static_cast<int>(p[4]) + static_cast<int>(p[5]))
                + (static_cast<int>(p[6]) + static_cast<int>(p[7]));
            
        }
        line_base += stride;
    }
	return sum / (width * height);
}


static int piperead(int fd, uint8_t *buf, int len)
{
   int n, r;

   r = 0;

   while(r<len)
   {
      n = read(fd,buf+r,len-r);
      if(n==0) return r;
      r += n;
   }
   return r;
}



static void read_chunk(void)
{
   int n, v, h, i,j, y;
   y4m_frame_info_t fi;


   for(j=0;j<READ_CHUNK_SIZE;++j)
   {
	   if( ctl_parallel_read )
	   {
		   // Unlock during the actual I/O filling buffers to allow
		   // the main thread to run if there are still the frames
		   // it needs.  The main thread atomically signals new_chunk_req 
		   // before waiting on new_chunk_ack.
		   // This thread atomically signals new_chunk_ack before
		   // waiting on new_chunk_req.
		   // Thus neither can suspend without first
		   // starting the other.
		   //mjpeg_info( "PRO:  releasing frame buf lock @ %d ", frames_read);

		   pthread_mutex_unlock( &frame_buffer_lock );
	   }
      n = frames_read % frame_buffer_size;

      y4m_init_frame_info (&fi);

      if ((y = y4m_read_frame_header (istrm_fd, &fi)) != Y4M_OK) 
	  {
		  if( y != Y4M_ERR_EOF )
			  mjpeg_log (LOG_WARN, 
						 "Error reading frame header (%d): code%s!", 
						 n,
						 y4m_strerr (n));
         goto EOF_MARK;
      }
      
      v = enc->parms.vertical_size;
      h = enc->parms.horizontal_size;
      for(i=0;i<v;i++)
         if(piperead(istrm_fd,frame_buffers[n][0]+i*enc->parms.phy_width,h)!=h) goto EOF_MARK;
	  lum_mean[n] = luminance_mean(frame_buffers[n][0] );

      v = CHROMA420==CHROMA420 ? 
		  enc->parms.vertical_size/2 : enc->parms.vertical_size;
      h = CHROMA420!=CHROMA444 ? 
		  enc->parms.horizontal_size/2 : enc->parms.horizontal_size;
      for(i=0;i<v;i++)
         if(piperead(istrm_fd,frame_buffers[n][1]+i*enc->parms.phy_chrom_width,h)!=h) goto EOF_MARK;
      for(i=0;i<v;i++)
         if(piperead(istrm_fd,frame_buffers[n][2]+i*enc->parms.phy_chrom_width,h)!=h) goto EOF_MARK;


	  if( ctl_parallel_read )
	  {
		  //
		  // Lock to atomically signal the availability of additional
		  // material from a chunk - waking the main thread
		  // if it suspended because a required frame was
		  // unavailable
		  //
		  //mjpeg_info( "PRO:  waiting for frame buf lock @ %d ", frames_read);
		  pthread_mutex_lock( &frame_buffer_lock );
	  }
	  ++frames_read;

	  if( ctl_parallel_read )
	  {
		  //mjpeg_info( "PRO: Signalling new_chunk_ack @ %d", frames_read );
		  pthread_cond_broadcast( &new_chunk_ack );
	  }

   }

   //
   // When we exit we're holding the lock again so we can
   // ensure we're waiting on new_chunk_req if the main thread is
   // currently running.
   //
   return;

   EOF_MARK:
   mjpeg_debug( "End of input stream detected" );
   if( ctl_parallel_read )
   {
	   pthread_mutex_lock( &frame_buffer_lock );
   }
   last_frame = frames_read-1;
   istrm_nframes = frames_read;
   //mjpeg_info( "Signalling last frame = %d", last_frame );
   if( ctl_parallel_read )
   {
	   //mjpeg_info( "PRO: Signalling new_chunk_ack @ %d", frames_read );
	   pthread_cond_broadcast( &new_chunk_ack );
   }

}




static void *read_chunks_worker(void *_dummy)
{
	//mjpeg_info("PRO: requesting frame buf lock" );
    //mjpeg_info( "PRO: has frame buf lock @ %d ", frames_read );
    //mjpeg_info( "PRO: Initial fill of frame buf" );
    pthread_mutex_lock( &frame_buffer_lock );
    read_chunk();
	for(;;)
	{
		//mjpeg_info( "PRO: has frame buf lock @ %d ", frames_read );
		//mjpeg_info( "PRO: Waiting for new_chunk_req " );
		pthread_cond_wait( &new_chunk_req, &frame_buffer_lock );
		//mjpeg_info( "PRO: new_chunk_req regained frame buf lock @  %d ", frames_read ); 
		if( frames_read < istrm_nframes ) 
		{
			read_chunk();
		}
	}
	return NULL;
}


static void start_worker(void)
{
	pthread_attr_t *pattr = NULL;

#ifdef HAVE_PTHREADSTACKSIZE
#define MINSTACKSIZE 200000
	pthread_attr_t attr;
	size_t stacksize;
	
	pthread_attr_init(&attr);
	pthread_attr_getstacksize(&attr, &stacksize);
	
	if (stacksize < MINSTACKSIZE) {
		pthread_attr_setstacksize(&attr, MINSTACKSIZE);
	}
	
	pattr = &attr;
#endif


	if( pthread_create( &worker_thread, pattr, read_chunks_worker, NULL ) != 0 )
	{
		mjpeg_error_exit1( "worker thread creation failed: %s", strerror(errno) );

	}

}

 /*****************************************************
 *
 *  Read another chunk of frames into the frame buffer if the
 *  specified frame is less than a chunk away from the end of the
 *  buffer.  This version is for when frame input reading is not
 *  multi-threaded and just goes ahead and does it.
 *
 * N.b. if ctl_parallel_read is active then read_chunk signals/locks
 * which could cause problems hence the assert!
 *
 *****************************************************/
   
static void read_chunk_seq( int num_frame )
{
    while(frames_read - num_frame < READ_CHUNK_SIZE && 
          frames_read < istrm_nframes ) 
    {
        read_chunk();
    }
}

 /*****************************************************
 *
 * Request read worker thread to read a chunk of frame into the frame
 * buffer if less than a chunk of frames is left after the specified
 * frame.  Wait for acknowledgement of the reading of a chunk (implying
 * at least one extra frame added to the buffer) if the specified frame
 * is not yet in the buffer.
 *
 * N.b. *must* be called with ctl_parallel_read active as otherwise it
 * will thoroughly deadlocked.
 *
 *****************************************************/
   


static void read_chunk_par( int num_frame)
{
	//mjpeg_info( "CON: requesting frame buf lock");
	pthread_mutex_lock( &frame_buffer_lock);
	for(;;)
	{
		//mjpeg_info( "CON: has frame buf lock @ %d (%d recorded read)", frames_read,  num_frame );
		// Activate reader process "on the fly"
		if( frames_read - num_frame < READ_CHUNK_SIZE && 
			frames_read < istrm_nframes )
		{
			//mjpeg_info( "CON: Running low on frames: signalling new_chunk_req" );

			pthread_cond_broadcast( &new_chunk_req );
		}
		if( frames_read > num_frame  || 
			frames_read >= istrm_nframes )
		{
			//mjpeg_info( "CON:  releasing frame buf lock - enough frames to go on with...");
			pthread_mutex_unlock( &frame_buffer_lock );
			return;
		}
		//mjpeg_info( "CON: waiting for new_chunk_ack - too few frames" );
		pthread_cond_wait( &new_chunk_ack, &frame_buffer_lock );
		//mjpeg_info( "CON: regained frame buf lock @ %d (%d processed)", frames_read,  num_frame );

	}
	
}

static void load_frame( int num_frame )
{
	if(last_frame>=0 && num_frame>last_frame &&num_frame<istrm_nframes)
	{
		mjpeg_error("Internal:readframe: internal error reading beyond end of frames");
		abort();
	}
	

   /* Read a chunk of frames if we've got less than one chunk buffered
	*/

   if( ctl_parallel_read )
	   read_chunk_par( num_frame );
   else
	   read_chunk_seq( num_frame );

   /* We aren't allowed to go too far behind the last read
	  either... */

   if( num_frame+static_cast<int>(frame_buffer_size) < frames_read )
   {
	   mjpeg_error("Internal: buffer flushed too soon req %d buffer %d..%d", num_frame, frames_read-frame_buffer_size, frames_read );
	   abort();
   }

	
}

int readframe( int num_frame,
               unsigned char *frame[]
	       	)
{
   int n;

   load_frame( num_frame ); 
   n = num_frame % frame_buffer_size;
   frame[0] = frame_buffers[n][0];
   frame[1] = frame_buffers[n][1];
   frame[2] = frame_buffers[n][2];

   return 0;
}

int frame_lum_mean( int num_frame )
{
	int n = num_frame;
    //
    // We use this function to probe for the existence of frames
    // so we clip at the end if the end is already found...
    //
	if(  last_frame > 0 && num_frame > last_frame )
	{
		n = last_frame;
	}
	load_frame( n );
    //
    // We may now know where the last frame is...
    //
    if( last_frame > 0 && n > last_frame )
        n = last_frame;
	return lum_mean[n% frame_buffer_size];
}


void read_stream_params( unsigned int *hsize, 
                         unsigned int *vsize, 
                         unsigned int *frame_rate_code,
                         unsigned int *interlacing_code,
                         unsigned int *aspect_ratio_code)
{
   int n;
   y4m_ratio_t sar;
   y4m_stream_info_t si;

   y4m_init_stream_info (&si);  
   if ((n = y4m_read_stream_header (istrm_fd, &si)) != Y4M_OK) {
       mjpeg_log(LOG_ERROR, "Could not read YUV4MPEG2 header: %s!",
                 y4m_strerr(n));
      exit (1);
   }

   *hsize = y4m_si_get_width(&si);
   *vsize = y4m_si_get_height(&si);
   *frame_rate_code = mpeg_framerate_code(y4m_si_get_framerate(&si));
   *interlacing_code = y4m_si_get_interlace(&si);

   /* Deduce MPEG aspect ratio from stream's frame size and SAR...
      (always as an MPEG-2 code; that's what caller expects). */
   sar = y4m_si_get_sampleaspect(&si);
   *aspect_ratio_code = mpeg_guess_mpeg_aspect_code(2, sar,
                                                    *hsize, *vsize);
}

/*********************
 *
 * Mark the border so that blocks in the frame are unlikely
 * to match blocks outside the frame.  This is useful for
 * motion estimation routines that, for speed, are a little
 * sloppy about some of the candidates they consider.
 *
 ********************/

static void border_mark( uint8_t *frame,
                         int w1, int h1, int w2, int h2)
{
  int i, j;
  uint8_t *fp;
  uint8_t mask = 0xff;
  /* horizontal pixel replication (right border) */
 
  for (j=0; j<h1; j++)
  {
    fp = frame + j*w2;
    for (i=w1; i<w2; i++)
    {
      fp[i] = mask;
      mask ^= 0xff;
    }
  }
 
  /* vertical pixel replication (bottom border) */

  for (j=h1; j<h2; j++)
  {
    fp = frame + j*w2;
    for (i=0; i<w2; i++)
    {
        fp[i] = mask;
        mask ^= 0xff;
    }
  }
}

void init_reader( )
{
#ifdef __linux__
    pthread_mutexattr_t mu_attr;
    pthread_mutexattr_t *p_attr = &mu_attr;
    pthread_mutexattr_settype( &mu_attr, PTHREAD_MUTEX_ERRORCHECK );
    
#else
    pthread_mutexattr_t *p_attr = NULL;		
#endif		
    pthread_mutex_init( &frame_buffer_lock, p_attr );

    mjpeg_info( "Buffering %d frames", frame_buffer_size );

    /* Allocate the frame data buffers: if we're not going to scan ahead
       for GOP size we can save a *lot* of memory... */
    if( ctl_N_max == ctl_N_min )
        frame_buffer_size = max(2*ctl_M,1)+READ_CHUNK_SIZE;
    else
        frame_buffer_size = 2*ctl_N_max+READ_CHUNK_SIZE;
    frame_buffers = (uint8_t ***) 
        bufalloc(frame_buffer_size*sizeof(uint8_t**));
	int n,i;
    for(n=0;n<frame_buffer_size;n++)
    {
        frame_buffers[n] = (uint8_t **) bufalloc(3*sizeof(uint8_t*));
        for (i=0; i<3; i++)
        {
                frame_buffers[n][i] = 
                    static_cast<uint8_t *>( bufalloc( (i==0) 
                                                      ? enc->parms.lum_buffer_size 
                                                      : enc->parms.chrom_buffer_size ) 
                        );
            }
        
        border_mark(frame_buffers[n][0],
                    enc->parms.enc_width,enc->parms.enc_height,
                    enc->parms.phy_width,enc->parms.phy_height);
        border_mark(frame_buffers[n][1],
                    enc->parms.enc_chrom_width, enc->parms.enc_chrom_height,
                    enc->parms.phy_chrom_width,enc->parms.phy_chrom_height);
        border_mark(frame_buffers[n][2],
                    enc->parms.enc_chrom_width, enc->parms.enc_chrom_height,
                    enc->parms.phy_chrom_width,enc->parms.phy_chrom_height);
        
    }
    
    lum_mean = new int[frame_buffer_size];

    /*
          Pre-fill the buffer 
        */
    if( ctl_parallel_read )
    {
        start_worker();
        read_chunk_par( frame_buffer_size/2 );
    }
    else
    {
        read_chunk_seq( frame_buffer_size/2);
    }
    
}

/* 
 * Local variables:
 *  c-file-style: "stroustrup"
 *  tab-width: 4
 *  indent-tabs-mode: nil
 * End:
 */
