/*
 * Sony Edge LZMA decompression implementation for SPURS LFQueue
 * Derived from Sony Edge 1.2.0 (LZMA SDK 4.40 by Igor Pavlov)
 */

#include "edge_lzma.h"
#include "cellSpurs.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef unsigned char Byte;
typedef unsigned short UInt16;
typedef unsigned int UInt32;
typedef size_t SizeT;
typedef UInt16 CProb;

#define LZMA_RESULT_OK 0
#define LZMA_RESULT_DATA_ERROR 1

#define LZMA_BASE_SIZE 1846
#define LZMA_LIT_SIZE 768
#define LZMA_PROPERTIES_SIZE 5

typedef struct
{
  int lc;
  int lp;
  int pb;
} CLzmaProperties;

#define LzmaGetNumProbs(Properties) (LZMA_BASE_SIZE + (LZMA_LIT_SIZE << ((Properties)->lc + (Properties)->lp)))

typedef struct
{
  CLzmaProperties Properties;
  UInt16 *Probs;
} CLzmaDecoderState;

static int LzmaDecodeProperties(CLzmaProperties *propsRes, const unsigned char *propsData, int size)
{
  unsigned char prop0;
  if (size < LZMA_PROPERTIES_SIZE)
    return LZMA_RESULT_DATA_ERROR;
  prop0 = propsData[0];
  if (prop0 >= 0xE1)
    return LZMA_RESULT_DATA_ERROR;
  {
    for (propsRes->pb = 0; prop0 >= 45; propsRes->pb++, prop0 -= 45);
    for (propsRes->lp = 0; prop0 >= 9; propsRes->lp++, prop0 -= 9);
    propsRes->lc = prop0;
  }
  return LZMA_RESULT_OK;
}

#define RC_TEST { if (Buffer == BufferLim) return LZMA_RESULT_DATA_ERROR; }





#define RC_TEST { if (Buffer == BufferLim) return LZMA_RESULT_DATA_ERROR; }



//#define Literal 0x736



//#if Literal != LZMA_BASE_SIZE

#if 0x736 != LZMA_BASE_SIZE

StopCompilingDueBUG

#endif



//#define kLzmaStreamWasFinishedId (-1)



int LzmaDecode(const CLzmaDecoderState *const vs,

    const unsigned char *const inStream, SizeT const inSize, SizeT *const inSizeProcessed,

    unsigned char *const outStream, SizeT const outSize, SizeT *const outSizeProcessed)

{

  CProb *const p = vs->Probs;

  SizeT nowPos = 0;

  Byte previousByte = 0;

  UInt32 const posStateMask = (1 << (vs->Properties.pb)) - 1;

  UInt32 const literalPosMask = (1 << (vs->Properties.lp)) - 1;

  const int lc = vs->Properties.lc;



  int state = 0;

  UInt32 rep0 = 1, rep1 = 1, rep2 = 1, rep3 = 1;

  // NOTE: this variable is initialized below before it is used, so it should not be initialized here

  //int len = 0;

  int len;

  const Byte *Buffer;

  const Byte *BufferLim;

  UInt32 Range;

  UInt32 Code;



  *inSizeProcessed = 0;

  *outSizeProcessed = 0;



  {

    UInt32 i;

    //DOWNCODE: 0x736 changed to 0x738 to ensure probLit in most frequent flow will be qword aligned

    const UInt32 numProbs = 0x738/*0x736*//*Literal*/ + ((UInt32)0x300/*LZMA_LIT_SIZE*/ << (lc + vs->Properties.lp));

    for (i = 0; i < numProbs; i++)

      p[i] = 0x800/*kBitModelTotal*/ >> 1;

  }

  

  //RC_INIT(inStream, inSize);

  Buffer = inStream;

  BufferLim = inStream + inSize;

  Code = 0;

  Range = 0xFFFFFFFF;

  { int i;

    for(i = 0; i < 5; i++)

    { RC_TEST;

      Code = (Code << 8) | (*Buffer++);

    }

  }



  while(nowPos < outSize)

  {

    // THIS IS MAIN LOOP!

    

    //count[0]++; // 0x046a8 observed

    CProb *prob;

    UInt32 bound;

    const int posState = (int)( nowPos & posStateMask);

    prob = p + 0/*IsMatch*/ + (state << 4/*kNumPosBitsMax*/) + posState;

    //IfBit0(prob)

    if (Range < 0x01000000/*kTopValue*/)

    { // this flow NOT usually taken

      //count[1]++; // 008c5 observed

      RC_TEST;

      Range <<= 8;

      Code = (Code << 8) | (*Buffer++);

    }

    // count of 0x046a8 observed

    bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

    if (Code < bound)



    { // this flow usually taken

      //count[2]++; // 0x03ca6 observed

      int symbol = 1;

      //UpdateBit0(prob)

      Range = bound;

      *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;



      //DOWNCODE: 0x736 changed to 0x738 to ensure probLit in most frequent flow will be qword aligned

      prob = p + 0x738/*0x736*//*Literal*/ + (0x300/*LZMA_LIT_SIZE*/ * ((( nowPos 

        & literalPosMask) << lc) + (previousByte >> (8 - lc))));

      if (state >= 7/*kNumLitStates*/)

      { // this flow NOT usually taken



        //count[3]++; // 0x00739 oserved

        int matchByte;

        matchByte = outStream[nowPos - rep0];

        do

        { 

          //count[4]++; // 0x02374 observed

          int bit;

          CProb *probLit;

          matchByte <<= 1;

          bit = (matchByte & 0x100);

          probLit = prob + 0x100 + bit + symbol;

          //RC_GET_BIT2(probLit, symbol, if (bit != 0) break, if (bit == 0) break)

          if (Range < 0x01000000/*kTopValue*/)

          { // this flow NOT usually taken 

            //count[5]++; // 0x0023b observed

            RC_TEST;

            Range <<= 8;

            Code = (Code << 8) | (*Buffer++);

          }

          // count 0x02374 observed

          bound = (Range >> 11/*kNumBitModelTotalBits*/) * *probLit;

          if (Code < bound)

          { // about 50-50, this flow NOT usually taken

            //count[6]++; // 0x0117c observed

            Range = bound;

            *probLit += (0x800/*kBitModelTotal*/ - *probLit) >> 5/*kNumMoveBits*/;

            symbol <<= 1;

            if (bit != 0)

            { // this flow NOT usually taken

              //count[7]++; // 0x003af observed

              break; // break goes go count10

            }

          }

          else

          { 

            //count[8]++; // 0x011f8 observed

            Range -= bound;

            Code -= bound;

            *probLit -= (*probLit) >> 5/*kNumMoveBits*/;

            symbol = (symbol + symbol) + 1;

            if (bit == 0)

            { // this flow NOT usually taken

              //count[9]++; // 0x00384 observed

              break; // break goes go count10

            }

          } 



        }while (symbol < 0x100); // branch to count4 usually taken

      }

      //count[10]++; // 03ca6 observed

      while (symbol < 0x100)

      { // THIS IS START OF MOST FREQUENT FLOW!

        //count[11]++; // 0x1c1bc observed (this is MAX!)

        CProb *const probLit = prob + symbol;

        //RC_GET_BIT(probLit, symbol)

        if (Range < 0x01000000/*kTopValue*/)

        { // this flow NOT usually taken

          //count[12]++; // 0x2810 observed

          RC_TEST;

          Range <<= 8;

          Code = (Code << 8) | (*Buffer++);

        }

        // 0x1c1bc observed (this is MAX!)

        bound = (Range >> 11/*kNumBitModelTotalBits*/) * *probLit;

        if (Code < bound)

        { // about 50-50, but NOT usually taken

          //count[13]++; // 0x0dd6d observed

          Range = bound;

          *probLit += (0x800/*kBitModelTotal*/ - *probLit) >> 5/*kNumMoveBits*/;

          symbol <<= 1;

        }

        else

        { 

          //count[14]++; // 0x0e44f observed

          Range -= bound;

          Code -= bound;

          *probLit -= (*probLit) >> 5/*kNumMoveBits*/;

          symbol = (symbol + symbol) + 1;

        }



      }//branch above to count11 uaually taken

      // THIS IS END OF MOST FREQUENT FLOW!

      //count14pt5

      // (0x03ca6 observed)

      previousByte = (Byte)symbol;

      outStream[nowPos++] = previousByte;

      if (state < 4)

      { // this flow usually taken

        //count[15]++; // 0x03150 observed

        state = 0;

      }

      else

      {

        //count[16]++; // 0x00b96 observed

        if (state < 10)

        { // this flow usually taken

          //count[17]++; // 0x009a0 observed

          state -= 3;

        }

        else

        {

          //count[18]++; // 0x001b6 observed

          state -= 6;

        }

      }

    }

    else             

    {

      //count[19]++; 0x00a02 observed

      //UpdateBit1(prob);

      Range -= bound;

      Code -= bound;

      *prob -= (*prob) >> 5/*kNumMoveBits*/;



      prob = p + 0xC0/*IsRep*/ + state;

      //IfBit0(prob)

      if (Range < 0x01000000/*kTopValue*/)

      { // this flow NOT usually taken

        //count[20]++; // 0x00258 observed

        RC_TEST;

        Range <<= 8;

        Code = (Code << 8) | (*Buffer++);

      }

      // (0x00a02 observed)

      bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

      if (Code < bound)



      { // this flow usually taken

        //count[21]++; // 0x0075d observed

        //UpdateBit0(prob);

        Range = bound;

        *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;



        rep3 = rep2;

        rep2 = rep1;

        rep1 = rep0;

        state = state < 7/*kNumLitStates*/ ? 0 : 3;

        prob = p + 0x332/*LenCoder*/;

      }

      else

      { // START OF 1st INFREQUENT FLOW!

        //count[22]++; // 0x002a5 observed

        //UpdateBit1(prob);

        Range -= bound;

        Code -= bound;

        *prob -= (*prob) >> 5/*kNumMoveBits*/;



        prob = p + 0xCC/*IsRepG0*/ + state;

        //IfBit0(prob)

        if (Range < 0x01000000/*kTopValue*/)

        { // this flow NOT usually taken

          //count[23]++; // 0x00071 observed

          RC_TEST;

          Range <<= 8;

          Code = (Code << 8) | (*Buffer++);

        }

        // (0x002a5 observed)

        bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

        if (Code < bound)



        { // this flow usually taken

          //count[24]++; // 0x00212 observed

          //UpdateBit0(prob);

          Range = bound;

          *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;



          prob = p + 0xF0/*IsRep0Long*/ + (state << 4/*kNumPosBitsMax*/) + posState;

          //IfBit0(prob)

          if (Range < 0x01000000/*kTopValue*/)

          { // this flow NOT usually taken

            //count[25]++; // 0x0002d

            RC_TEST;

            Range <<= 8;

            Code = (Code << 8) | (*Buffer++);

          }

          // (0x00212 observed)

          bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

          if (Code < bound)



          { // this flow usually taken

            //count[26]++; 0x00129 observed

            //UpdateBit0(prob);

            Range = bound;

            *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;

            

            if (nowPos == 0)

              return LZMA_RESULT_DATA_ERROR;

            state = state < 7/*kNumLitStates*/ ? 9 : 11;

            previousByte = outStream[nowPos - rep0];

            outStream[nowPos++] = previousByte;

            continue;

          }

          else

          {

            //count[27]++; // 0x000e9 observed

            //UpdateBit1(prob);

            Range -= bound;

            Code -= bound;

            *prob -= (*prob) >> 5/*kNumMoveBits*/;



          }

        }

        else

        {

          //count[28]++; // 0x00093 observed

          UInt32 distance;

          //UpdateBit1(prob);

          Range -= bound;

          Code -= bound;

          *prob -= (*prob) >> 5/*kNumMoveBits*/;



          prob = p + 0xD8/*IsRepG1*/ + state;

          //IfBit0(prob)

          if (Range < 0x01000000/*kTopValue*/)

          { // this flow NOT usually taken

            //count[29]++; // 0x00028 observed

            RC_TEST;

            Range <<= 8;

            Code = (Code << 8) | (*Buffer++);

          }

          // (0x00093 observed)

          bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

          if (Code < bound)



          { // this flow usually taken

            //count[30]++; // 0x00050 observed

            //UpdateBit0(prob);

            Range = bound;

            *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;



            distance = rep1;

          }

          else 

          {

            //count[31]++; // 0x00043 observed

            //UpdateBit1(prob);

            Range -= bound;

            Code -= bound;

            *prob -= (*prob) >> 5/*kNumMoveBits*/;



            prob = p + 0xE4/*IsRepG2*/ + state;

            //IfBit0(prob)

            if (Range < 0x01000000/*kTopValue*/)

            { // this flow NOT usually taken

              //count[32]++; // 0x0000d observed

              RC_TEST;

              Range <<= 8;

              Code = (Code << 8) | (*Buffer++);

            }

            // (0x00043 observed)

            bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob);

            if (Code < bound)



            { // this flow usually taken

              //count[33]++; 0x00029 observed

              //UpdateBit0(prob);

              Range = bound;

              *(prob) += (0x800/*kBitModelTotal*/ - *(prob)) >> 5/*kNumMoveBits*/;



              distance = rep2;

            }

            else

            {

              //count[34]++; 0x0001a observed

              //UpdateBit1(prob);

              Range -= bound;

              Code -= bound;

              *prob -= (*prob) >> 5/*kNumMoveBits*/;



              distance = rep3;

              rep3 = rep2;

            }

            //count34.3

            // (0x00043 observed)

            rep2 = rep1;

          }

          //count34.5

          // (0x00093 observed)

          rep1 = rep0;

          rep0 = distance;

        }

        //count[69]++; // 0x0017c observed

        state = state < 7/*kNumLitStates*/ ? 8 : 11;

        prob = p + 0x534/*RepLenCoder*/;

      }// END OF 1st INFREQUENT FLOW

      //count[35]++; 0x008d9 observed

      {

        int numBits, offset;

        CProb *probLen = prob + 0/*LenChoice*/;

        //IfBit0(probLen)

        if (Range < 0x01000000/*kTopValue*/)

        { // this flow NOT usually taken

          //count[36]++; // 0x0007c observed

          RC_TEST;

          Range <<= 8;

          Code = (Code << 8) | (*Buffer++);

        }

        // (0x008d9 observed)

        bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(probLen);

        if (Code < bound)



        { // this flow usually taken

          //count[37]++; // 0x00809 observed

          //UpdateBit0(probLen);

          Range = bound;

          *(probLen) += (0x800/*kBitModelTotal*/ - *(probLen)) >> 5/*kNumMoveBits*/;



          probLen = prob + 2/*LenLow*/ + (posState << 3/*kLenNumLowBits*/);

          offset = 0;

          numBits = 3/*kLenNumLowBits*/;

        }

        else

        { // START OF 2nd INFREQUENT FLOW!

          //count[38]++; 0x000d0 observed

          //UpdateBit1(probLen);

          Range -= bound;

          Code -= bound;

          *probLen -= (*probLen) >> 5/*kNumMoveBits*/;



          probLen = prob + 1/*LenChoice2*/;

          //IfBit0(probLen)

          if (Range < 0x01000000/*kTopValue*/)

          { // this flow NOT usually taken

            //count[39]++; 0x0003e observed

            RC_TEST;

            Range <<= 8;

            Code = (Code << 8) | (*Buffer++);

          }

          // (0x000d0 observed)

          bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(probLen);

          if (Code < bound)



          { // this flow NOT usually taken

            //count[40]++; // 0x00007 observed

            //UpdateBit0(probLen);

            Range = bound;

            *(probLen) += (0x800/*kBitModelTotal*/ - *(probLen)) >> 5/*kNumMoveBits*/;



            probLen = prob + 0x82/*LenMid*/ + (posState << 3/*kLenNumMidBits*/);

            offset = 8/*kLenNumLowSymbols*/;

            numBits = 3/*kLenNumMidBits*/;

          }

          else

          {

            //count[41]++; // 0x000c9 observed

            //UpdateBit1(probLen);

            Range -= bound;

            Code -= bound;

            *probLen -= (*probLen) >> 5/*kNumMoveBits*/;



            probLen = prob + 0x102/*LenHigh*/;

            offset = 8/*kLenNumLowSymbols*/ + 8/*kLenNumMidSymbols*/;

            numBits = 8/*kLenNumHighBits*/;

          }

        } // END OF 2nd INFREQUENT FLOW!

        //count[70]++; // 0x008d9 observed

        //RangeDecoderBitTreeDecode(probLen, numBits, len);

        { int i = numBits;

          len = 1;

          do

          { 

            //count[42]++; // 0x01e78 observed

            CProb *const prob1 = probLen + len;  // Note: p changed to prob1 to avoid shadowing main p

            if (Range < 0x01000000/*kTopValue*/)

            { // this flow NOT usually taken

              //count[43]++; // 0x00187 observed

              RC_TEST;

              Range <<= 8;

              Code = (Code << 8) | (*Buffer++);

            }

            // (0x01e78 observed)

            bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob1);

            if (Code < bound)

            { // this flow usually taken

              //count[44]++; // 0x011ed observed

              Range = bound;

              *(prob1) += (0x800/*kBitModelTotal*/ - *(prob1)) >> 5/*kNumMoveBits*/;

              len <<= 1;

            }

            else

            { 

              //count[45]++; // 0x00c8b observed

              Range -= bound;

              Code -= bound;

              *(prob1) -= (*(prob1)) >> 5/*kNumMoveBits*/;

              len = (len + len) + 1;

            }

          }while(--i != 0); // branch above usually taken

          //count45.3

          // (0x008d9 observed)

          len -= (1 << numBits);

        }



		//count45.5

        // (0x008d9 observed)

        len += offset;

      }

      //count[46]++; // 0x008d9 observed

      if (state < 4)

      { // this code usually taken

        //count[47]++; // 0x0075d observed

        int posSlot;

        state += 7/*kNumLitStates*/;

        prob = p + 0x1B0/*PosSlot*/ +

            ((len < 4/*kNumLenToPosStates*/ ? len : 4/*kNumLenToPosStates*/ - 1) << 6/*kNumPosSlotBits*/);

        //RangeDecoderBitTreeDecode(prob, kNumPosSlotBits, posSlot);

        { 

          int i = 6/*kNumPosSlotBits*/;

          posSlot = 1;

          // (0x0075d observed)

          do

          { 

            //count[48]++; // 0x02c2e observed

            CProb *const prob2 = prob + posSlot;  // Note: p changed to prob2 to avoid shadowing main p

            if (Range < 0x01000000/*kTopValue*/)

            { // this flow NOT usually taken

              //count[49]++; 0x00341 observed

              RC_TEST;

              Range <<= 8;

              Code = (Code << 8) | (*Buffer++);

            }

            // (0x02c2e observed)

            bound = (Range >> 11/*kNumBitModelTotalBits*/) * *(prob2);

            if (Code < bound)

            { // almost 50-50, this code NOT usually taken

              //count[50]++; // 0x01585 observed

              Range = bound;

              *(prob2) += (0x800/*kBitModelTotal*/ - *(prob2)) >> 5/*kNumMoveBits*/;

              posSlot <<= 1;

            }

            else

            { 

              //count[51]++; // 0x016a9 observed

              Range -= bound;

              Code -= bound;

              *(prob2) -= (*(prob2)) >> 5/*kNumMoveBits*/;

              posSlot = (posSlot + posSlot) + 1;

            }

          }while(--i != 0); // branch above usually taken

          // (0x0075d observed)          

          posSlot -= (1 << 6/*kNumPosSlotBits*/);

        }



        //count[52]++; // 0x0075d observed

        if (posSlot >= 4/*kStartPosModelIndex*/)

        { // this flow usually taken

          //count[53]++; // 0x006bc observed

          int numDirectBits = ((posSlot >> 1) - 1);

          rep0 = (2 | ((UInt32)posSlot & 1));

          if (posSlot < 14/*kEndPosModelIndex*/)

          { // this flow NOT usually taken

            //count[54]++; // 0x000bc observed

            // This is called the 3rd non-frequent flow

            rep0 <<= numDirectBits;

            prob = p + 0x2B0/*SpecPos*/ + rep0 - posSlot - 1;

            // end of the 3rd non-frequent flow

          }

          else

          {

            //count[55]++; // 0x00600 observed

            numDirectBits -= 4/*kNumAlignBits*/;

            do

            {

              //count[56]++; // 0x2e6a observed

              //RC_NORMALIZE

              if (Range < 0x01000000/*kTopValue*/)

              { // this flow NOT usually taken

                //count[57]++; // 0x005c8 observed

                RC_TEST;

                Range <<= 8;

                Code = (Code << 8) | (*Buffer++);

              }

              

              // (0x2e6a observed)

              Range >>= 1;

              rep0 <<= 1;

              if (Code >= Range)

              { // this flow usually taken

                //count[58]++; // 0x01835 observed

                Code -= Range;

                rep0 |= 1;

              }

            }while (--numDirectBits != 0); // branch above usually taken

            // (0x00600 observed)

            prob = p + 0x322/*Align*/;

            rep0 <<= 4/*kNumAlignBits*/;

            numDirectBits = 4/*kNumAlignBits*/;

          }

          //count[59]++; // 0x006bc observed

          {

            int i = 1;

            int mi = 1;

            do

            {

              //count[60]++; // 0x19ea observed

              CProb *const prob3 = prob + mi;

              //RC_GET_BIT2(prob3, mi, ; , rep0 |= i);

              if (Range < 0x01000000/*kTopValue*/)

              { // this flow NOT usually taken

                //count[61]++; 0x001ac observed

                RC_TEST;

                Range <<= 8;

                Code = (Code << 8) | (*Buffer++);

              }

              // (0x19ea observed)

              bound = (Range >> 11/*kNumBitModelTotalBits*/) * *prob3;

              if (Code < bound)

              { // this flow NOT usually observed

                //count[62]++; // 0x00624 observed

                Range = bound;

                *prob3 += (0x800/*kBitModelTotal*/ - *prob3) >> 5/*kNumMoveBits*/;

                mi <<= 1;

              }

              else

              { 

                //count[63]++; // 0x013c6 observed

                Range -= bound;

                Code -= bound;

                *prob3 -= (*prob3) >> 5/*kNumMoveBits*/;

                mi = (mi + mi) + 1;

                rep0 |= i;

              }



              // (0x19ea observed)

              i <<= 1;

            }while(--numDirectBits != 0); // branch above usually taken

          }

        }

        else

        {

          //count[64]++; // 0x000a1 observed

          rep0 = posSlot;

        }

        //count[65]++; // 0x0075d observed

        if (++rep0 == (UInt32)(0))

        { // this flow NOT usually taken

		

          //count[66]++; // 0x00000 observed

          /* it's for stream version */

          len = -1/*kLzmaStreamWasFinishedId*/;

          break;

        }

      }

      //count[67]++; // 0x008d9 observed

      len += 2/*kMatchMinLen*/;

      if (rep0 > nowPos)

        return LZMA_RESULT_DATA_ERROR;

      do

      {

        //count[68]++; // 0x0c231 observed

        previousByte = outStream[nowPos - rep0];

        len--;

        outStream[nowPos++] = previousByte;

      }while(len != 0 && nowPos < outSize); // branch above usually taken

    }

  }//usually branch above to main loop via "while(nowPos < outSize)"



  //RC_NORMALIZE;

  if (Range < 0x01000000/*kTopValue*/)

  { RC_TEST;

    // NOTE: Range and Code (below) are not used after this, so this code is removed

    // Only Buffer++ needs to be done since it is used afterwards

    //Range <<= 8;

    //Code = (Code << 8) | (*Buffer++);

    Buffer++;

  }



  *inSizeProcessed = (SizeT)(Buffer - inStream);

  *outSizeProcessed = nowPos;

  return LZMA_RESULT_OK;



}



#ifdef _WIN32
static CRITICAL_SECTION s_edge_lzma_cs;
static int s_edge_lzma_init = 0;
static void edge_lzma_lock(void) {
    if (!s_edge_lzma_init) {
        InitializeCriticalSection(&s_edge_lzma_cs);
        s_edge_lzma_init = 1;
    }
    EnterCriticalSection(&s_edge_lzma_cs);
}
static void edge_lzma_unlock(void) {
    LeaveCriticalSection(&s_edge_lzma_cs);
}
#else
static pthread_mutex_t s_edge_lzma_lock = PTHREAD_MUTEX_INITIALIZER;
static void edge_lzma_lock(void) { pthread_mutex_lock(&s_edge_lzma_lock); }
static void edge_lzma_unlock(void) { pthread_mutex_unlock(&s_edge_lzma_lock); }
#endif

static UInt16 s_lzma_probs[32768];
static uint8_t s_lzma_out_buf[0x10000 + 256];

s32 edge_lzma_process_element(uint64_t buffer_ea)
{
    if (!vm_base || buffer_ea == 0) return CELL_OK;
    uint32_t b_ea = (uint32_t)buffer_ea;

    uint32_t eaCompressed                = vm_read32(b_ea + 0x00);
    uint32_t eaUncompressed              = vm_read32(b_ea + 0x04);
    uint32_t compressedSize              = vm_read32(b_ea + 0x08);
    uint32_t outputUncompPartialBuffSize = vm_read32(b_ea + 0x0C);
    uint32_t eaWorkToDoCounter           = vm_read32(b_ea + 0x10);
    uint32_t eaEventFlag                 = vm_read32(b_ea + 0x14);
    uint16_t eventFlagBits               = vm_read16(b_ea + 0x18);
    uint16_t outputUncompSkipBeginSize   = vm_read16(b_ea + 0x1A);
    uint16_t outputUncompSkipEndSize     = vm_read16(b_ea + 0x1C);
    uint8_t  properties0                 = vm_read8(b_ea + 0x1E);

    uint32_t isCompressed = (eaWorkToDoCounter & 1u);
    uint32_t counterEa    = eaWorkToDoCounter & ~1u;

    uint32_t expectedUncompSize = (uint32_t)outputUncompSkipBeginSize + 
                                  outputUncompPartialBuffSize + 
                                  (uint32_t)outputUncompSkipEndSize;

    static int s_push_cnt = 0;
    if (s_push_cnt++ < 20 || (s_push_cnt % 100) == 0) {
        fprintf(stderr, "[edge_lzma] #%d: compEA=0x%08X uncompEA=0x%08X compSz=%u outPartSz=%u expSz=%u isComp=%u ctrEA=0x%08X efEA=0x%08X efBits=0x%04X\n",
                s_push_cnt, eaCompressed, eaUncompressed, compressedSize,
                outputUncompPartialBuffSize, expectedUncompSize, isCompressed,
                counterEa, eaEventFlag, eventFlagBits);
    }

    int err = 0;
    edge_lzma_lock();

    if (isCompressed) {
        if (expectedUncompSize <= sizeof(s_lzma_out_buf) && compressedSize <= 0x10000) {
            uint8_t fakedProperties[5] = { properties0, 0x00, 0x00, 0x01, 0x00 };
            CLzmaDecoderState state;
            if (LzmaDecodeProperties(&state.Properties, fakedProperties, 5) != LZMA_RESULT_OK) {
                fprintf(stderr, "[edge_lzma] LzmaDecodeProperties error for prop0=0x%02X\n", properties0);
                err = 1;
            } else {
                state.Probs = s_lzma_probs;
                SizeT inProcessed = 0;
                SizeT outProcessed = 0;
                const unsigned char* inStream = (const unsigned char*)vm_ptr8(eaCompressed);
                err = LzmaDecode(&state,
                                 inStream, compressedSize, &inProcessed,
                                 s_lzma_out_buf, expectedUncompSize, &outProcessed);
                if (err != LZMA_RESULT_OK || outProcessed != expectedUncompSize) {
                    fprintf(stderr, "[edge_lzma] LzmaDecode error (%d), inProc=%zu outProc=%zu exp=%u\n",
                            err, inProcessed, outProcessed, expectedUncompSize);
                    err = 1;
                } else {
                    memcpy(vm_ptr8(eaUncompressed),
                           s_lzma_out_buf + outputUncompSkipBeginSize,
                           outputUncompPartialBuffSize);
                }
            }
        } else {
            fprintf(stderr, "[edge_lzma] unexpected sizes: compSz=%u expSz=%u\n",
                    compressedSize, expectedUncompSize);
            err = 1;
        }
    } else {
        if (compressedSize == expectedUncompSize) {
            memcpy(vm_ptr8(eaUncompressed),
                   (const uint8_t*)vm_ptr8(eaCompressed) + outputUncompSkipBeginSize,
                   outputUncompPartialBuffSize);
        } else {
            fprintf(stderr, "[edge_lzma] uncompressed size mismatch: compSz=%u expSz=%u\n",
                    compressedSize, expectedUncompSize);
            err = 1;
        }
    }

    /* Counter decrement and event flag */
    int setEventFlag = 1;
    if (counterEa != 0) {
        setEventFlag = 0;
        uint32_t val = vm_read32(counterEa);
        if (err != 0) {
            val |= 0x80000000u;
        }
        uint32_t count = val & ~0x80000000u;
        if (count > 0) {
            count--;
            vm_write32(counterEa, (val & 0x80000000u) | count);
            if (count == 0) {
                setEventFlag = 1;
            }
        }
    }

    edge_lzma_unlock();

    if (setEventFlag && eaEventFlag != 0) {
        cellSpursEventFlagSet((CellSpursEventFlag*)(uintptr_t)eaEventFlag, eventFlagBits);
    }

    return CELL_OK;
}
