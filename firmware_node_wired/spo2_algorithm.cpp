/** \file algorithm.cpp ******************************************************
*
* Project: MAXREFDES117#
* Filename: algorithm.cpp
* Description: This module calculates the heart rate/SpO2 level
*
*
* --------------------------------------------------------------------
*
* This code follows the following naming conventions:
*
* char              ch_pmod_value
* char (array)      s_pmod_s_string[16]
* float             f_pmod_value
* int32_t           n_pmod_value
* int32_t (array)   an_pmod_value[16]
* int16_t           w_pmod_value
* int16_t (array)   aw_pmod_value[16]
* uint16_t          uw_pmod_value
* uint16_t (array)  auw_pmod_value[16]
* uint8_t           uch_pmod_value
* uint8_t (array)   auch_pmod_buffer[16]
* uint32_t          un_pmod_value
* int32_t *         pn_pmod_value
*
* ------------------------------------------------------------------------- */
/*******************************************************************************
* Copyright (C) 2016 Maxim Integrated Products, Inc., All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL MAXIM INTEGRATED BE LIABLE FOR ANY CLAIM, DAMAGES
* OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*
* Except as contained in this notice, the name of Maxim Integrated
* Products, Inc. shall not be used except as stated in the Maxim Integrated
* Products, Inc. Branding Policy.
*
* The mere transfer of this software does not imply any licenses
* of trade secrets, proprietary technology, copyrights, patents,
* trademarks, maskwork rights, or any other form of intellectual
* property whatsoever. Maxim Integrated Products, Inc. retains all
* ownership rights.
*******************************************************************************
*/

#include "Arduino.h"
#include "spo2_algorithm.h"

#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
//Arduino Uno doesn't have enough SRAM to store 100 samples of IR led data and red led data in 32-bit format
//To solve this problem, 16-bit MSB of the sampled data will be truncated.  Samples become 16-bit data.
void maxim_heart_rate_and_oxygen_saturation(uint16_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint16_t *pun_red_buffer, int32_t *pn_spo2, int8_t *pch_spo2_valid, 
                int32_t *pn_heart_rate, int8_t *pch_hr_valid)
#else
void maxim_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer, int32_t *pn_spo2, int8_t *pch_spo2_valid, 
                int32_t *pn_heart_rate, int8_t *pch_hr_valid)
#endif
/**
* \brief        Calculate the heart rate and SpO2 level
* \par          Details
*               By detecting  peaks of PPG cycle and corresponding AC/DC of red/infra-red signal, the an_ratio for the SPO2 is computed.
*               Since this algorithm is aiming for Arm M0/M3. formaula for SPO2 did not achieve the accuracy due to register overflow.
*               Thus, accurate SPO2 is precalculated and save longo uch_spo2_table[] per each an_ratio.
*
* \param[in]    *pun_ir_buffer           - IR sensor data buffer
* \param[in]    n_ir_buffer_length      - IR sensor data buffer length
* \param[in]    *pun_red_buffer          - Red sensor data buffer
* \param[out]    *pn_spo2                - Calculated SpO2 value
* \param[out]    *pch_spo2_valid         - 1 if the calculated SpO2 value is valid
* \param[out]    *pn_heart_rate          - Calculated heart rate value
* \param[out]    *pch_hr_valid           - 1 if the calculated heart rate value is valid
*
* \retval       None
*/
{
  uint32_t un_ir_mean;
  int32_t k, n_i_ratio_count;
  int32_t i, n_exact_ir_valley_locs_count, n_middle_idx;
  int32_t n_th1, n_npks;   
  int32_t an_ir_valley_locs[15] ;
  
  int32_t n_y_ac, n_x_ac;
  int32_t n_spo2_calc; 
  int32_t n_y_dc_max, n_x_dc_max; 
  int32_t n_y_dc_max_idx = 0;
  int32_t n_x_dc_max_idx = 0; 
  int32_t an_ratio[10], n_ratio_average; 

  // calculates DC mean and subtract DC from ir
  un_ir_mean =0; 
  for (k=0 ; k<n_ir_buffer_length ; k++ ) un_ir_mean += pun_ir_buffer[k] ;
  un_ir_mean =un_ir_mean/n_ir_buffer_length ;
    
  // remove DC and invert signal so that we can use peak detector as valley detector
  for (k=0 ; k<n_ir_buffer_length ; k++ )  
    an_x[k] = (int32_t)un_ir_mean - (int32_t)pun_ir_buffer[k]; 
    
  // 4 pt Moving Average
  for(k=0; k< BUFFER_SIZE-MA4_SIZE; k++){
    an_x[k]=( an_x[k]+an_x[k+1]+ an_x[k+2]+ an_x[k+3])/(int)4;        
  }
  // Adaptive peak threshold from mean absolute AC amplitude (prevents noise triggers on strong PPG)
  int64_t abs_sum = 0;
  for (k = 0; k < BUFFER_SIZE - MA4_SIZE; k++) {
    abs_sum += (an_x[k] >= 0) ? an_x[k] : -an_x[k];
  }
  n_th1 = (int32_t)((abs_sum / (BUFFER_SIZE - MA4_SIZE)) / 2);
  if (n_th1 < 30) n_th1 = 30;
  if (n_th1 > 250) n_th1 = 250;

  for ( k=0 ; k<15;k++) an_ir_valley_locs[k]=0;
  // Min peak distance = 8 samples @ 25Hz (0.32s => max 187 BPM) to reject dicrotic notches
  maxim_find_peaks( an_ir_valley_locs, &n_npks, an_x, BUFFER_SIZE - MA4_SIZE, n_th1, 8, 15 );

  int32_t an_ibi[15];
  int32_t n_ibi_count = 0;
  if (n_npks >= 3) {
    for (k = 1; k < n_npks; k++) {
      int32_t diff = an_ir_valley_locs[k] - an_ir_valley_locs[k - 1];
      if (diff >= 8 && diff <= 38) {
        an_ibi[n_ibi_count++] = diff;
      }
    }
  }
  if (n_ibi_count >= 2) {
    maxim_sort_ascend(an_ibi, n_ibi_count);
    int32_t median_ibi = an_ibi[n_ibi_count / 2];
    int32_t tol = (median_ibi / 4 > 3) ? (median_ibi / 4) : 3;
    int32_t valid_sum = 0;
    int32_t valid_cnt = 0;
    for (k = 0; k < n_ibi_count; k++) {
      int32_t delta = an_ibi[k] - median_ibi;
      if (delta < 0) delta = -delta;
      if (delta <= tol) {
        valid_sum += an_ibi[k];
        valid_cnt++;
      }
    }
    if (valid_cnt >= 2 && valid_sum > 0) {
      *pn_heart_rate = (int32_t)(((int64_t)FreqS * 60 * valid_cnt + (valid_sum / 2)) / valid_sum);
      *pch_hr_valid = 1;
    } else {
      *pn_heart_rate = -999;
      *pch_hr_valid = 0;
    }
  } else {
    *pn_heart_rate = -999;
    *pch_hr_valid = 0;
  }

  //  load raw value again for SPO2 calculation : RED(=y) and IR(=X)
  for (k=0 ; k<n_ir_buffer_length ; k++ )  {
      an_x[k] =  pun_ir_buffer[k] ; 
      an_y[k] =  pun_red_buffer[k] ; 
  }

  // find precise min near an_ir_valley_locs
  n_exact_ir_valley_locs_count =n_npks; 
  
  n_ratio_average =0; 
  n_i_ratio_count = 0; 
  for(k=0; k< 10; k++) an_ratio[k]=0;
  for (k=0; k< n_exact_ir_valley_locs_count; k++){
    if (an_ir_valley_locs[k] >= BUFFER_SIZE ){
      *pn_spo2 =  -999 ;
      *pch_spo2_valid  = 0; 
      return;
    }
  }
  for (k=0; k< n_exact_ir_valley_locs_count-1; k++){
    n_y_dc_max= -16777216 ; 
    n_x_dc_max= -16777216; 
    int32_t span = an_ir_valley_locs[k+1] - an_ir_valley_locs[k];
    if (span >= 8 && span <= 38){
      for (i=an_ir_valley_locs[k]; i< an_ir_valley_locs[k+1]; i++){
        if (an_x[i]> n_x_dc_max) {n_x_dc_max =an_x[i]; n_x_dc_max_idx=i;}
        if (an_y[i]> n_y_dc_max) {n_y_dc_max =an_y[i]; n_y_dc_max_idx=i;}
      }
      n_y_ac= (an_y[an_ir_valley_locs[k+1]] - an_y[an_ir_valley_locs[k]]) * (n_y_dc_max_idx - an_ir_valley_locs[k]);
      n_y_ac=  an_y[an_ir_valley_locs[k]] + n_y_ac / span;
      n_y_ac=  an_y[n_y_dc_max_idx] - n_y_ac;
      n_x_ac= (an_x[an_ir_valley_locs[k+1]] - an_x[an_ir_valley_locs[k]]) * (n_x_dc_max_idx - an_ir_valley_locs[k]);
      n_x_ac=  an_x[an_ir_valley_locs[k]] + n_x_ac / span;
      // Fix original Maxim bug: sample an_x at n_x_dc_max_idx (not n_y_dc_max_idx)
      n_x_ac=  an_x[n_x_dc_max_idx] - n_x_ac;

      // Use 64-bit arithmetic to avoid 32-bit overflow on 18-bit MAX30102 readings
      if (n_y_ac > 0 && n_x_ac > 0 && n_x_dc_max > 0 && n_y_dc_max > 0 && n_i_ratio_count < 10) {
        int64_t n_nume64 = (int64_t)n_y_ac * (int64_t)n_x_dc_max;
        int64_t n_denom64 = (int64_t)n_x_ac * (int64_t)n_y_dc_max;
        int64_t r100 = (n_nume64 * 100) / n_denom64;
        if (r100 > 2 && r100 < 184) {
          an_ratio[n_i_ratio_count++] = (int32_t)r100;
        }
      }
    }
  }
  if (n_i_ratio_count < 2) {
    *pn_spo2 = -999;
    *pch_spo2_valid = 0;
    return;
  }
  maxim_sort_ascend(an_ratio, n_i_ratio_count);
  n_middle_idx = n_i_ratio_count / 2;

  if (n_i_ratio_count % 2 == 0)
    n_ratio_average = (an_ratio[n_middle_idx - 1] + an_ratio[n_middle_idx]) / 2;
  else
    n_ratio_average = an_ratio[n_middle_idx];

  if (n_ratio_average > 2 && n_ratio_average < 184) {
    n_spo2_calc = uch_spo2_table[n_ratio_average];
    *pn_spo2 = n_spo2_calc;
    *pch_spo2_valid = 1;
  } else {
    *pn_spo2 = -999;
    *pch_spo2_valid = 0;
  }
}


void maxim_find_peaks( int32_t *pn_locs, int32_t *n_npks,  int32_t  *pn_x, int32_t n_size, int32_t n_min_height, int32_t n_min_distance, int32_t n_max_num )
/**
* \brief        Find peaks
* \par          Details
*               Find at most MAX_NUM peaks above MIN_HEIGHT separated by at least MIN_DISTANCE
*
* \retval       None
*/
{
  maxim_peaks_above_min_height( pn_locs, n_npks, pn_x, n_size, n_min_height );
  maxim_remove_close_peaks( pn_locs, n_npks, pn_x, n_min_distance );
  *n_npks = min( *n_npks, n_max_num );
}

void maxim_peaks_above_min_height( int32_t *pn_locs, int32_t *n_npks,  int32_t  *pn_x, int32_t n_size, int32_t n_min_height )
/**
* \brief        Find peaks above n_min_height
* \par          Details
*               Find all peaks above MIN_HEIGHT
*
* \retval       None
*/
{
  int32_t i = 1, n_width;
  *n_npks = 0;
  
  while (i < n_size-1){
    if (pn_x[i] > n_min_height && pn_x[i] > pn_x[i-1]){      // find left edge of potential peaks
      n_width = 1;
      while (i+n_width < n_size && pn_x[i] == pn_x[i+n_width])  // find flat peaks
        n_width++;
      if (pn_x[i] > pn_x[i+n_width] && (*n_npks) < 15 ){      // find right edge of peaks
        pn_locs[(*n_npks)++] = i;    
        // for flat peaks, peak location is left edge
        i += n_width+1;
      }
      else
        i += n_width;
    }
    else
      i++;
  }
}

void maxim_remove_close_peaks(int32_t *pn_locs, int32_t *pn_npks, int32_t *pn_x, int32_t n_min_distance)
/**
* \brief        Remove peaks
* \par          Details
*               Remove peaks separated by less than MIN_DISTANCE
*
* \retval       None
*/
{
    
  int32_t i, j, n_old_npks, n_dist;
    
  /* Order peaks from large to small */
  maxim_sort_indices_descend( pn_x, pn_locs, *pn_npks );

  for ( i = -1; i < *pn_npks; i++ ){
    n_old_npks = *pn_npks;
    *pn_npks = i+1;
    for ( j = i+1; j < n_old_npks; j++ ){
      n_dist =  pn_locs[j] - ( i == -1 ? -1 : pn_locs[i] ); // lag-zero peak of autocorr is at index -1
      if ( n_dist > n_min_distance || n_dist < -n_min_distance )
        pn_locs[(*pn_npks)++] = pn_locs[j];
    }
  }

  // Resort indices int32_to ascending order
  maxim_sort_ascend( pn_locs, *pn_npks );
}

void maxim_sort_ascend(int32_t  *pn_x, int32_t n_size) 
/**
* \brief        Sort array
* \par          Details
*               Sort array in ascending order (insertion sort algorithm)
*
* \retval       None
*/
{
  int32_t i, j, n_temp;
  for (i = 1; i < n_size; i++) {
    n_temp = pn_x[i];
    for (j = i; j > 0 && n_temp < pn_x[j-1]; j--)
        pn_x[j] = pn_x[j-1];
    pn_x[j] = n_temp;
  }
}

void maxim_sort_indices_descend(  int32_t  *pn_x, int32_t *pn_indx, int32_t n_size)
/**
* \brief        Sort indices
* \par          Details
*               Sort indices according to descending order (insertion sort algorithm)
*
* \retval       None
*/ 
{
  int32_t i, j, n_temp;
  for (i = 1; i < n_size; i++) {
    n_temp = pn_indx[i];
    for (j = i; j > 0 && pn_x[n_temp] > pn_x[pn_indx[j-1]]; j--)
      pn_indx[j] = pn_indx[j-1];
    pn_indx[j] = n_temp;
  }
}




