/* ratectl.c, bitrate control routines (linear quantization only currently) */

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

#include <config.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

#include "global.h"
#include "fastintfns.h"

/* private prototypes */

static double calc_actj ( pict_data_s *picture);

/*Constant bit-rate rate control variables */
/* X's global complexity (Chi! not X!) measures.
   Actually: average quantisation * bits allocated in *previous* frame
   N.b. the choice of measure is *not* arbitrary.  The feedback bit rate
   control gets horribly messed up if it is *not* proportionate to bit demand
   i.e. bits used scaled for quantisation.
   d's  virtual reciever buffer fullness
   r - Rate control feedback gain (in bits/frame)  I.e if the "virtual
   buffer" is x% full and
*/

double Xi = 0, Xp = 0, Xb = 0;
int r;
int	d0i = 0, d0pb = 0, d0p = 0, d0b = 0;


/* R - Remaining bits available in the next one second period.
   T - Target bits for current frame 
   d - Current d for quantisation purposes updated using 
   scaled difference of target bit usage and actual usage
   NOTE: We use double's for these as 48 bits will suffice and
   there's some tricky float arithmetic required/

*/
static int32_t R;
static int64_t R_cum = 0.0;
static int64_t e_R_cum = 0.0;
static int64_t gop_start;
static int64_t gop_bits = 0;
static int64_t per_frame_bits;
static int64_t per_gop_bits;
static int64_t bits_transported;
static int64_t bits_used;
static int32_t buffer_variation = 0;
static int d;
static int32_t T;

/* bitcnt_EOP - Position in generated bit-stream for latest
				end-of-picture Comparing these values with the
				bit-stream position for when the picture is due to be
				displayed allows us to see what the vbv buffer is up
				to.

*/

static int64_t bitcnt_EOP = 0;
static int frame_overshoot_margin = 0;
static int undershoot_carry;
static double undershoot_gain;
static double overshoot_gain;

/*
  actsum - Total activity (sum block variances) in frame
  actcovered - Activity macroblocks so far quantised (used to
               fine tune quantisation to avoid starving highly
			   active blocks appearing late in frame...) UNUSED
  avg_act - Current average activity...
*/
static double actsum;
static double actcovered;
static double sum_avg_act = 0.0;
static double avg_act;


static int Np, Nb;
static int64_t S;

/* Note: eventually we may wish to tweak these to suit image content */
static double Ki;    	/* Down-scaling of I/B/P-frame complexity */
static double Kb;	    /* relative to others in bit-allocation   */
static double Kp;	    /* calculations.  We only need 2 but have all
							   3 for readability */


static int min_d,max_d;
static int min_q, max_q;

static double avg_KI = 5.0;	/* TODO: These values empirically determined 		*/
static double avg_KB = 10.0*2.0;	/* for MPEG-1, may need tuning for MPEG-2	*/
static double avg_KP = 10.0*2.0;
#define K_AVG_WINDOW_I 4.0
#define K_AVG_WINDOW_P   10.0
#define K_AVG_WINDOW_B   20.0
static double bits_per_mb;

static double SQ = 0.0;
static double sum_vbuf_Q = 0.0;
static int fast_tune = 0;
static int first_gop = 1;
static int first_B = 0;
static int first_P = 0;
static int first_I = 0;

/* Scaling and clipping of quantisation factors
   One floating point used in predictive calculations.  
   The second integerises the results of the first for use in the
   actual stream...
*/


static double scale_quantf( pict_data_s *picture, double quant )
{
	double quantf;

	if ( picture->q_scale_type )
	{
		int iquantl, iquanth;
		double wl, wh;

		/* BUG TODO: This should interpolate the table... */
		wh = quant-floor(quant);
		wl = 1.0 - wh;
		iquantl = (int) floor(quant);
		iquanth = iquantl+1;
		/* clip to legal (linear) range */
		if (iquantl<1)
		{
			iquantl = 1;
			iquanth = 1;
		}
		
		if (iquantl>111)
		{
			iquantl = 112;
			iquanth = 112;
		}
		
		quantf = (double)
		  wl * (double)non_linear_mquant_table[map_non_linear_mquant[iquantl]] 
			+ 
		  wh * (double)non_linear_mquant_table[map_non_linear_mquant[iquanth]]
			;
	}
	else
	{
		/* clip mquant to legal (linear) range */
		quantf = quant;
		if (quantf<2.0)
			quantf = 2;
		if (quantf>62.0)
			quantf = 62.0;
	}
	return quantf;
}

static int scale_quant( pict_data_s *picture, double quant )
{
	int iquant;
	
	if ( picture->q_scale_type  )
	{
		iquant = (int) floor(quant+0.5);

		/* clip mquant to legal (linear) range */
		if (iquant<1)
			iquant = 1;
		if (iquant>112)
			iquant = 112;

		iquant =
			non_linear_mquant_table[map_non_linear_mquant[iquant]];
	}
	else
	{
		/* clip mquant to legal (linear) range */
		iquant = (int)floor(quant+0.5);
		if (iquant<2)
			iquant = 2;
		if (iquant>62)
			iquant = 62;
		iquant = (iquant/2)*2; // Must be *even*
	}
	return iquant;
}

/*********************

 Initialise rate control parameters
 params:  reinit - Rate control is being re-initialised during the middle
                   of a run.  Don't reset adaptive parameters.
 ********************/

void rc_init_seq(int reinit)
{

	/* No bits unused from previous GOP....*/
	gop_start = 0;
	bits_transported = 0;
	bits_used = 0;
	/* If its stills with a size we have to hit then make the
	   guesstimates of for initial quantisation pessimistic...
	*/
	if( opt_still_size > 0 )
	{
		avg_KI *= 1.5;
		per_frame_bits = opt_still_size * 8;
		R = opt_still_size * 8;
	}
	else
	{
		per_frame_bits = opt_bit_rate / opt_frame_rate;
		R = opt_bit_rate;
		/* We assume buffer is nicely topped up when we start */
		bits_transported += undershoot_carry;
	}

	e_R_cum += (gop_start+per_gop_bits-bitcount());


	/* Everything else already set or adaptive */
	if( reinit )
		return;

	first_gop = 1;

	/* Calculate reasonable margins for variation in the decoder buffer.
	   we assume that having less than 5 frame intervals worth buffered
	   is cutting it fine for avoiding under-runs.

	   The gain values represent the fraction of the under/over shoot
	   to be recovered during one second.  Gain is decreased if the buffer margin
	   is large, gain is higher for avoiding overshoot.

	   Currently, for a 1-frame sized margin gain is set to recover
	   an undershoot in half a second
	*/
	if( opt_still_size > 0 )
	{
		undershoot_carry = 0;
		undershoot_gain = 1.0;
		overshoot_gain = 1.0;
	}
	else
	{
		int buffer_safe = 5 * per_frame_bits ;
		undershoot_carry = (ctl_video_buffer_size - buffer_safe);
		if( undershoot_carry < per_frame_bits/2 )
			mjpeg_error_exit1( "Buffer appears to be set too small (< a frames variation possible)\n" );
		undershoot_gain = 3.0 * (46*1024 * 8) / ctl_video_buffer_size;
		overshoot_gain = 1.5 * undershoot_gain;
		
	}
	bits_per_mb = (double)opt_bit_rate / (mb_per_pict);

	/* reaction parameter (constant) decreased to increase response
	   rate as encoder is currently tending to under/over-shoot... in
	   rate   */
	if (r==0)  
		r = (int)floor(4.0*opt_bit_rate/opt_frame_rate);

	Ki = 1.2;
	Kb = 1.3;
	Kp = 1.2;

	/* Heuristic: In constant bit-rate streams we assume buffering
	   will allow us to pre-load some (probably small) fraction of the
	   buffers size worth of following data if earlier data was
	   undershot its bit-rate allocation
	 
	*/


	d0i = 0;					/* Force initial quuantisation to be */
	d0pb = 0;					/* predice from image complexity */
	d0p = 0;
	d0b = 0;

#ifdef OUTPUT_STAT
	fprintf(statfile,"\nrate control: sequence initialization\n");
	fprintf(statfile,
			" initial global complexity measures (I,P,B): Xi=%.0f, Xp=%.0f, Xb=%.0f\n",
			Xi, Xp, Xb);
	fprintf(statfile," reaction parameter: r=%d\n", r);
	fprintf(statfile,
			" initial virtual buffer fullness (I,P,B): d0i=%d, d0pb=%d",
			d0i, d0pb);
#endif


}

static double T_sum;

void rc_init_GOP( int np, int nb)
{
	Np = opt_fieldpic ? 2*np+1 : np;
	Nb = opt_fieldpic ? 2*nb : nb;

	if( opt_still_size > 0 )
		per_gop_bits = per_frame_bits;
	else
		per_gop_bits = (double)(1 + np + nb) * per_frame_bits;

	
	/* A.Stevens Aug 2000: at last I've found the wretched
	   rate-control overshoot bug...  Simply "topping up" R here means
	   that we can accumulate an indefinately large pool of bits
	   "saved" from previous low-activity frames.  This is of
	   course nonsense.

	   In CBR we can only accumulate as much as our buffer allows,
	   after that the eventual system stream will have to be padded.
	   The automatic padding will make this calculation fairly
	   reasonable but since that's based on estimates we again impose
	   our rough and ready heuristic that we can't accumulate more
	   than approximately half a video buffer full.

	   In VBR we actually do nothing different.  Here the bitrate is
	   simply a ceiling rate which we expect to undershoot a lot as
	   our quantisation floor cuts in. We specify a great big buffer
	   and simply don't pad when we undershoot.

	   However, we don't want to carry over absurd undershoots as when it
	   does get hectic we'll breach our maximum.
		
	   TODO: For CBR we should do a proper video buffer model and use
	   it to make bit allocation decisions.

	*/

	T_sum = 0.0;
	R_cum = (R-per_gop_bits);

	gop_start = bitcount();


	if( first_gop )
	{
		fast_tune = 1;
		first_I = first_B = first_P = 1;
		first_gop = 0;
	}
	else
	{
		fast_tune = 0;
	}
#ifdef OUTPUT_STAT
	fprintf(statfile,"\nrate control: new group of pictures (GOP)\n");
	fprintf(statfile," target number of bits for GOP: R=%.0f\n",R);
	fprintf(statfile," number of P pictures in GOP: Np=%d\n",Np);
	fprintf(statfile," number of B pictures in GOP: Nb=%d\n",Nb);
#endif
}


/* Step 1: compute target bits for current picture being coded */
void rc_init_pict(pict_data_s *picture)
{
	double avg_K = 0.0;
	double target_Q;
	double current_Q;
	double Si, Sp, Sb;
	int available_bits;
	int frames_in_gop = Nb+Np+1;
	/* TODO: A.Stevens  Nov 2000 - This modification needs testing visually.

	   Weird.  The original code used the average activity of the
	   *previous* frame as the basis for quantisation calculations for
	   rather than the activity in the *current* frame.  That *has* to
	   be a bad idea..., surely, here we try to be smarter by using the
	   current values and keeping track of how much of the frames
	   activitity has been covered as we go along.

	   We also guesstimate the relationship between  (sum
	   of DCT coefficients) and actual quantisation weighted activty.
	   We use this to try to predict the activity of each frame.
	*/
	
	actsum =  calc_actj(picture );
	avg_act = (double)actsum/(double)(mb_per_pict);
	sum_avg_act += avg_act;
	actcovered = 0.0;
	sum_vbuf_Q = 0.0;


	/* Allocate target bits for frame based on frames numbers in GOP
	   weighted by:
	   - global complexity averages
	   - predicted activity measures
	   - fixed type based weightings
	   
	   T = (Nx * Xx/Kx) / Sigma_j (Nj * Xj / Kj)

	   N.b. B frames are an exception as there is *no* predictive
	   element in their bit-allocations.  The reason this is done is
	   that highly active B frames are inevitably the result of
	   transients and/or scene changes.  Psycho-visual considerations
	   suggest there's no point rendering sudden transients
	   terribly well as they're not percieved accurately anyway.  

	   In the case of scene changes similar considerations apply.  In
	   this case also we want to save bits for the next I or P frame
	   where they will help improve other frames too.  
	   TODO: Experiment with *inverse* predictive correction for B-frames
	   and turning off active-block boosting for B-frames.

	   Note that we have to calulate per-frame bits by scaling the one-second
	   bit-pool a one-GOP bit-pool.
	*/


	if( opt_still_size > 0 )
		available_bits = per_frame_bits;
	else if( buffer_variation > 0 )
	{
		available_bits = 
			(R+buffer_variation*undershoot_gain)*frames_in_gop/opt_frame_rate;
	}
	else
	{
		available_bits = 
			(R+buffer_variation*overshoot_gain)*frames_in_gop/opt_frame_rate;
	}

	min_q = min_d = INT_MAX;
	max_q = max_d = INT_MIN;
	switch (picture->pict_type)
	{
	case I_TYPE:
		
		/* There is little reason to rely on the *last* I-frame
		   as they're not closely related.  The slow correction of
		   K should be enough to fine-tune...
		*/

		d = d0i;
		avg_K = avg_KI;
		Si = avg_K*actsum;
		if( first_I )
		{
			T = (int32_t)(available_bits/(1.0+(Np/1.7)+Nb/(2.0*1.7)));
			first_I = 0;
		}
		else
			T =(int32_t)(available_bits/(1.0+Np*Xp*Ki/(Si*Kp)+Nb*Xb*Ki/(Si*Kb)));
		break;
	case P_TYPE:
		d = d0p;
		avg_K = avg_KP;
		Sp = avg_K * actsum;
		if( first_P )
		{
			T = (int32_t)(available_bits/(Np+Nb/2.0));
		}
		else
			T =  (int32_t)(available_bits/(Np+Nb*Kp*Xb/(Kb*Sp)));
		break;
	case B_TYPE:
		d = d0b;
		avg_K = avg_KB;
		Sb = avg_K * actsum;
		if( first_B )
		{
			T =  (int32_t)(available_bits/(Nb+Np*2.0));
		}
		else
			T =  (int32_t)(available_bits/(Nb+Np*Kb*Xp/(Kp*Sb)));
		break;
	}


	/* Undershot bits have been "returned" via R */
	if( d < 0 )
		d = 0;

	/* We don't let the target volume get absurdly low as it makes some
	   of the prediction maths ill-condtioned.  At these levels quantisation
	   is always minimum anyway
	*/
	T = intmax( T, 4000 );

	if( opt_still_size > 0 && opt_vbv_buffer_still_size )
	{
		/* If stills size must match then target low to ensure no
		   overshoot.
		*/
		mjpeg_info( "Setting overshoot margin for T=%d\n", T/8 );
		frame_overshoot_margin = T/16;
		T -= frame_overshoot_margin;
	}

	//printf( "(%d,%d):R=%d T=%d ", Nb, Np,R/8, T/8);

	T_sum += T;
	target_Q = scale_quantf(picture, 
							avg_K * avg_act *(mb_per_pict) / T);
	current_Q = scale_quant(picture,62.0*d / r);
	picture->avg_act = avg_act;
	picture->sum_avg_act = sum_avg_act;

	/*  If guesstimated target quantisation requirement is much different
		than that suggested by the virtual buffer we reset the virtual
		buffer to set the guesstimate as initial quantisation.

		This effectively speeds up rate control response when sudden
		changes in content occur
	*/


	if ( 62.0*d / r  < target_Q / 2.0  )
	{
		d = (int) (target_Q  * r / (62.0));
	}

	if ( 62.0*d / r  > target_Q * 2.0  )
	{
		d = (int) (d+(target_Q  * r / 62.0))/2;
	}

	S = bitcount();


#ifdef OUTPUT_STAT
	fprintf(statfile,"\nrate control: start of picture\n");
	fprintf(statfile," target number of bits: T=%.0f\n",T/8);
#endif
}



static double calc_actj(pict_data_s *picture)
{
	int i,j,k,l;
	double actj,sum;
	uint16_t *i_q_mat;
	int actsum;
	sum = 0.0;
	k = 0;

	for (j=0; j<height2; j+=16)
		for (i=0; i<width; i+=16)
		{
			/* A.Stevens Jul 2000 Luminance variance *has* to be a
			   rotten measure of how active a block in terms of bits
			   needed to code a lossless DCT.  E.g. a half-white
			   half-black block has a maximal variance but pretty
			   small DCT coefficients.

			   So.... instead of luminance variance as used in the
			   original we use the absolute sum of DCT coefficients as
			   our block activity measure.  */

			if( picture->mbinfo[k].mb_type  & MB_INTRA )
			{
				i_q_mat = i_intra_q;
				/* Compensate for the wholly disproprotionate weight
				 of the DC coefficients.  Shold produce more sensible
				 results...  yes... it *is* an mostly empirically derived
				 fudge factor ;-)
				*/
				actsum =  -80*COEFFSUM_SCALE;
			}
			else
			{
				i_q_mat = i_inter_q;
				actsum = 0;
			}

			/* It takes some bits to code even an entirely zero block...
			   It also makes a lot of calculations a lot better conditioned
			   if it can be guaranteed that activity is always distinctly
			   non-zero.
			 */


			for( l = 0; l < 6; ++l )
				actsum += 
					(*pquant_weight_coeff_sum)
					    ( picture->mbinfo[k].dctblocks[l], i_q_mat ) ;
			actj = (double)actsum / (double)COEFFSUM_SCALE;
			if( actj < 12.0 )
				actj = 12.0;

			picture->mbinfo[k].act = (double)actj;
			sum += (double)actj;
			++k;
		}
	return sum;
}

/*
 * Update rate-controls statistics after pictures has ended..
 *
 */
void rc_update_pict(pict_data_s *picture)
{
	double X;
	double K;
	double AQ;
	int64_t AP,PP;		/* Actual and padded picture bit counts */
	int    i;
	int    Qsum;
	int frame_overshoot;
	
	AP = bitcount() - S;
	frame_overshoot = (int)AP-(int)T;

	/* For the virtual buffers for quantisation feedback it is the
	   actual under/overshoot *including* padding.  Otherwise the
	   buffers go zero.
	*/
	d += frame_overshoot;

	/* Warn if it looks like we've busted the safety margins in stills
	   size specification.  Adjust padding to account for safety
	   margin if we're padding to suit stills whose size has to be
	   specified in advance in vbv_buffer_size.
	*/
	picture->pad = 0;

	if( opt_still_size > 0 && opt_vbv_buffer_still_size)
	{
		int padding_bytes;
		if( frame_overshoot > frame_overshoot_margin )
		{
			mjpeg_warn( "Rate overshoot: VCD hi-res still %d bytes too large! \n", 
						((int)AP)/8-opt_still_size);
		}
		
		//
		// Aim for an actual size squarely in the middle of the 2048
		// byte granuality of the still_size coding.  This gives a 
		// safety margin for headers etc.
		//
		frame_overshoot = frame_overshoot - frame_overshoot_margin;
		if( frame_overshoot < -2048*8 )
			frame_overshoot += 1024*8;

		padding_bytes = -frame_overshoot/8;
		if( padding_bytes > 0 )
		{
			mjpeg_debug( "Padding still to size: %d extra bytes\n", padding_bytes );
			picture->pad = 1;
			alignbits();
			for( i = 0; i < padding_bytes/2; ++i )
			{
				putbits(0, 16);
			}
		}
		AP = bitcount() - S;			/* total # of bits in picture */
	}
	
	bits_used += AP;
	bits_transported += per_frame_bits;
	buffer_variation  = (int32_t)(bits_transported - bits_used);

	/* In VBR we simply stop reading once our buffer is filled up */
	   
	if( ctl_quant_floor != 0 && buffer_variation > undershoot_carry )
	{
		bits_transported = bits_used + undershoot_carry;
		buffer_variation = undershoot_carry;	
	}

	/* Anything more than the buffer is likely to be able to hold
	   will be simply padded away by the multiplexer. 
	*/

	if( buffer_variation > undershoot_carry )
	{
		bits_used += (buffer_variation -undershoot_carry);
		PP = AP;
		buffer_variation = undershoot_carry;
	}

	else
		PP = AP;

	
	   
	R = (R-PP)+per_frame_bits;		/* remaining # of bits over a  GOP length*/

	/* Make sure our 1-second bit-pool never grows larger than our nominal
	   bit-rate plus our maximum carry margin
	*/

	R = intmin( opt_bit_rate+undershoot_carry, R );

	Qsum = 0;
	for( i = 0; i < mb_per_pict; ++i )
	{
		Qsum += picture->mbinfo[i].mquant;
	}

	
    /* AQ is the average Quantisation of the block.
	   Its only used for stats display as the integerisation
	   of the quantisation value makes it rather coarse for use in
	   estimating bit-demand */
	AQ = (double)Qsum/(double)mb_per_pict;
	SQ += AQ;
	
	/* X (Chi - Complexity!) is an estimate of "bit-demand" for the
	frame.  I.e. how many bits it would need to be encoded without
	quantisation.  It is used in adaptively allocating bits to busy
	frames. It is simply calculated as bits actually used times
	average target (not rounded!) quantisation.

	K is a running estimate of how bit-demand relates to frame
	activity - bits demand per activity it is used to allow
	prediction of quantisation needed to hit a bit-allocation.
	*/

	X = AP  * AQ; //sum_vbuf_Q /  mb_per_pict;
	K = X / actsum;
	picture->AQ = AQ;
	picture->SQ = SQ;

	gop_bits += per_frame_bits;

	mjpeg_debug( "D=%lld R=%d dB=%d\n", 
				(gop_bits - bitcount())/8, (int)R/8,
				buffer_variation/8  );
	
	/* Xi are used as a guesstimate of *typical* frame activities
	   based on the past.  Thus we don't want anomalous outliers due
	   to scene changes swinging things too much (this is handled by
	   the predictive complexity measure stuff) so we use moving
	   averages.  The weightings are intended so all 3 averages have
	   similar real-time decay periods based on an assumption of
	   20-30Hz frame rates.
	*/

	switch (picture->pict_type)
	{
	case I_TYPE:
		avg_KI = (K + avg_KI * K_AVG_WINDOW_I) / (K_AVG_WINDOW_I+1.0) ;
		d0i = d;
		if( first_I )
			Xi = X;
		else
			Xi = (X + 2.0*Xi)/3.0;
		break;
	case P_TYPE:
		avg_KP = (K + avg_KP * K_AVG_WINDOW_P) / (K_AVG_WINDOW_P+1.0) ;
		d0p = d;
		if( first_P )
		{
			Xp = X;
			first_P = 0;
		}
		else if( fast_tune )
			Xp = (X+Xp*2.0)/3.0;
		else
			Xp = (X + Xp*K_AVG_WINDOW_P)/(K_AVG_WINDOW_P+1.0);
		/* Np--;*/
		break;
	case B_TYPE:
		avg_KB = (K + avg_KB * K_AVG_WINDOW_B) / (K_AVG_WINDOW_B+1.0) ;
		d0b = d;
		if( first_B )
		{
			Xb = X;
			first_B = 0;
		}
		else if( fast_tune )
		{
			Xb = (X + Xb * 3.0) / 4.0;
		}
		else
			Xb = (X + Xb*K_AVG_WINDOW_B)/(K_AVG_WINDOW_B+1.0);
		/* Nb--; */
		break;
	}

#ifdef OUTPUT_STAT
	fprintf(statfile,"\nrate control: end of picture\n");
	fprintf(statfile," actual number of bits: S=%lld\n",S);
	fprintf(statfile," average quantization parameter AQ=%.1f\n",
			(double)AQ);
	fprintf(statfile," remaining number of bits in GOP: R=%.0f\n",R);
	fprintf(statfile,
			" global complexity measures (I,P,B): Xi=%.0f, Xp=%.0f, Xb=%.0f\n",
			Xi, Xp, Xb);
	fprintf(statfile,
			" virtual buffer fullness (I,PB): d0i=%d, d0b=%d\n",
			d0i, d0b);
	fprintf(statfile," remaining number of P pictures in GOP: Np=%d\n",Np);
	fprintf(statfile," remaining number of B pictures in GOP: Nb=%d\n",Nb);
	fprintf(statfile," average activity: avg_act=%.1f \n", avg_act );
#endif

}

/* compute initial quantization stepsize (at the beginning of picture) 
   ctl_quant_floor != 0 is the VBR case where we set a bitrate as a (high)
   maximum and then put a floor on quantisation to achieve a reasonable
   overall size.
 */
int rc_start_mb(pict_data_s *picture)
{
	
	int mquant = scale_quant( picture, d*62.0/r );
	mquant = intmax(mquant, ctl_quant_floor);

/*
  fprintf(statfile,"rc_start_mb:\n");
  fprintf(statfile,"mquant=%d\n",mquant);
*/
	return mquant;
}

/* Step 2: measure virtual buffer - estimated buffer discrepancy */
int rc_calc_mquant( pict_data_s *picture,int j)
{
	int mquant;
	double dj, Qj, actj, N_actj; 

	/* A.Stevens 2000 : we measure how much *information* (total activity)
	   has been covered and aim to release bits in proportion.  Indeed,
	   complex blocks get an disproprortionate boost of allocated bits.
	   To avoid visible "ringing" effects...

	*/

	actj = picture->mbinfo[j].act;
	/* Guesstimate a virtual buffer fullness based on
	   bits used vs. bits in proportion to activity encoded
	*/


	dj = ((double)d) + 
		((double)(bitcount()-S) - actcovered * ((double)T) / actsum);



	/* scale against dynamic range of mquant and the bits/picture count.
	   ctl_quant_floor != 0.0 is the VBR case where we set a bitrate as a (high)
	   maximum and then put a floor on quantisation to achieve a reasonable
	   overall size.
	   Not that this *is* baseline quantisation.  Not adjust for local activity.
	   Otherwise we end up blurring active macroblocks. Silly in a VBR context.
	*/

	Qj = dj*62.0/r;

	Qj = (Qj > ctl_quant_floor) ? Qj : ctl_quant_floor;
	/*  Heuristic: Decrease quantisation for blocks with lots of
		sizeable coefficients.  We assume we just get a mess if
		a complex texture's coefficients get chopped...
		We don't bother for B frames as the effect can only be transient, 
		and hence not really noticeable.  The bits can be better "spent"
		on improving P and I frames.
	*/
		
	N_actj =  ( actj < avg_act || picture->pict_type == B_TYPE ) ? 
		1.0 : 
		(actj + ctl_act_boost*avg_act)/(ctl_act_boost*actj +  avg_act);

	sum_vbuf_Q += scale_quantf(picture,Qj*N_actj);
	mquant = scale_quant(picture,Qj*N_actj);


	/* Update activity covered */

	actcovered += actj;
	 
#ifdef OUTPUT_STAT
/*
  fprintf(statfile,"MQ(%d): ",j);
  fprintf(statfile,"dj=%.0f, Qj=%1.1f, actj=3.1%f, N_actj=1.1%f, mquant=%03d\n",
  dj,Qj,actj,N_actj,mquant);
*/
	picture->mbinfo[j].N_act = N_actj;
#endif
	return mquant;
}


/* VBV calculations
 *
 * generates warnings if underflow or overflow occurs
 */

/* vbv_end_of_picture
 *
 * - has to be called directly after writing picture_data()
 * - needed for accurate VBV buffer overflow calculation
 * - assumes there is no byte stuffing prior to the next start code
 *
 * Note correction for bytes that will be stuffed away in the eventual CBR
 * bit-stream.
 */

void vbv_end_of_picture(pict_data_s *picture)
{
	bitcnt_EOP = (bitcount()) - BITCOUNT_OFFSET;

	/* A.Stevens why bother... accuracy to 7 bits... ha ha!
	vbitcnt_EOP = (vbitcnt_EOP + 7) & ~7; 
	*/
}

/* calc_vbv_delay
 *
 * has to be called directly after writing the picture start code, the
 * reference point for vbv_delay
 *
 * A.Stevens 2000: 
 * Actually we call it just before the start code is written, but anyone
 * who thinks 32 bits +/- in all these other approximations matters is fooling
 * themselves.
 */

void calc_vbv_delay(pict_data_s *picture)
{
	static double picture_delay;
	static double next_ip_delay = 0.0; /* due to frame reordering delay */
	static double decoding_time = 0.0;

	
	/* number of 1/90000 s ticks until next picture is to be decoded */
	if (picture->pict_type == B_TYPE)
	{
		if (opt_prog_seq)
		{
			if (!picture->repeatfirst)
				picture_delay = 90000.0/opt_frame_rate; /* 1 frame */
			else
			{
				if (!picture->topfirst)
					picture_delay = 90000.0*2.0/opt_frame_rate; /* 2 frames */
				else
					picture_delay = 90000.0*3.0/opt_frame_rate; /* 3 frames */
			}
		}
		else
		{
			/* interlaced */
			if (opt_fieldpic)
				picture_delay = 90000.0/(2.0*opt_frame_rate); /* 1 field */
			else
			{
				if (!picture->repeatfirst)
					picture_delay = 90000.0*2.0/(2.0*opt_frame_rate); /* 2 flds */
				else
					picture_delay = 90000.0*3.0/(2.0*opt_frame_rate); /* 3 flds */
			}
		}
	}
	else
	{
		/* I or P picture */
		if (opt_fieldpic)
		{
			if(picture->topfirst && (picture->pict_struct==TOP_FIELD))
			{
				/* first field */
				picture_delay = 90000.0/(2.0*opt_frame_rate);
			}
			else
			{
				/* second field */
				/* take frame reordering delay into account */
				picture_delay = next_ip_delay - 90000.0/(2.0*opt_frame_rate);
			}
		}
		else
		{
			/* frame picture */
			/* take frame reordering delay into account*/
			picture_delay = next_ip_delay;
		}

		if (!opt_fieldpic || 
			picture->topfirst!=(picture->pict_struct==TOP_FIELD))
		{
			/* frame picture or second field */
			if (opt_prog_seq)
			{
				if (!picture->repeatfirst)
					next_ip_delay = 90000.0/opt_frame_rate;
				else
				{
					if (!picture->topfirst)
						next_ip_delay = 90000.0*2.0/opt_frame_rate;
					else
						next_ip_delay = 90000.0*3.0/opt_frame_rate;
				}
			}
			else
			{
				if (opt_fieldpic)
					next_ip_delay = 90000.0/(2.0*opt_frame_rate);
				else
				{
					if (!picture->repeatfirst)
						next_ip_delay = 90000.0*2.0/(2.0*opt_frame_rate);
					else
						next_ip_delay = 90000.0*3.0/(2.0*opt_frame_rate);
				}
			}
		}
	}

	if (decoding_time==0.0)
	{
		/* first call of calc_vbv_delay */
		/* we start with a 7/8 filled VBV buffer (12.5% back-off) */
		picture_delay = ((opt_vbv_buffer_size*7)/8)*90000.0/opt_bit_rate;
		if (opt_fieldpic)
			next_ip_delay = (int)(90000.0/opt_frame_rate+0.5);
	}

	/* VBV checks */

	/*
	   TODO: This is currently disabled because it is hopeless wrong
	   most of the time. It generates 20 warnings for frames with small
	   predecessors (small bitcnt_EOP) that in reality would be padded
	   away by the multiplexer for every realistic warning for an
	   oversize packet.
	*/

#ifdef CRIES_WOLF

	/* check for underflow (previous picture).
	*/
	if (!opt_low_delay && (decoding_time < (double)bitcnt_EOP*90000.0/opt_bit_rate))
	{
		/* picture not completely in buffer at intended decoding time */
		mjpeg_warn("vbv_delay underflow frame %d (target=%.1f, actual=%.1f\n)",
				   frame_num-1, decoding_time, bitcnt_EOP*90000.0/opt_bit_rate);
	}


	/* when to decode current frame */
	decoding_time += picture_delay;


	/* check for overflow (current picture).  Unless verbose warn
	   only if overflow must be at least in part due to an oversize
	   frame (rather than undersize predecessor).
	   	*/

	picture->vbv_delay = (int)(decoding_time - ((double)bitcnt_EOP)*90000.0/bit_rate);

	if ( decoding_time * ((double)bit_rate  / 90000.0) - ((double)bitcnt_EOP)
		> vbv_buffer_size )
	{
		double oversize = opt_vbv_buffer_size -
			(decoding_time / 90000.0 * bit_rate - (double)(bitcnt_EOP+frame_undershoot));
		if(!quiet || oversize > 0.0  )
			mjpeg_warn("vbv_delay overflow frame %d - %f.0 bytes!\n", 
					   frame_num,
					   oversize / 8.0
				);
	}


#ifdef OUTPUT_STAT
	fprintf(statfile,
			"\nvbv_delay=%d (bitcount=%lld, decoding_time=%.2f, bitcnt_EOP=%lld)\n",
			picture->vbv_delay,bitcount(),decoding_time,bitcnt_EOP);
#endif

	if (picture->vbv_delay<0)
	{
		mjpeg_warn("vbv_delay underflow: %d\n",picture->vbv_delay);
		picture->vbv_delay = 0;
	}



	if (picture->vbv_delay>65535)
	{
		mjpeg_warn("vbv_delay frame %d exceeds permissible range: %d\n",
				   frame_num, picture->vbv_delay);
		picture->vbv_delay = 65535;
	}
#else
	if( !opt_mpeg1 || ctl_quant_floor != 0 || opt_still_size > 0)
		picture->vbv_delay =  0xffff;
	else if( opt_still_size > 0 )
		picture->vbv_delay =  90000.0/opt_frame_rate/4;
#endif

}
