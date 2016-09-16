/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkImageMutualInformation.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkImageMutualInformation.h"

#include "vtkObjectFactory.h"
#include "vtkMath.h"
#include "vtkImageData.h"
#include "vtkImageStencilData.h"
#include "vtkImageStencilIterator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkMultiThreader.h"
#include "vtkTemplateAliasMacro.h"
#include "vtkPointData.h"
#include "vtkVersion.h"

#if VTK_MAJOR_VERSION > 7 || (VTK_MAJOR_VERSION == 7 && VTK_MINOR_VERSION >= 0)
#define USE_SMP_THREADED_IMAGE_ALGORITHM
#include "vtkSMPTools.h"
#endif

#include <math.h>

vtkStandardNewMacro(vtkImageMutualInformation);

#ifdef USE_SMP_THREADED_IMAGE_ALGORITHM
//----------------------------------------------------------------------------
template<class T, class TLS>
class vtkImageSimilarityMetricTLSIterator
{
public:
  vtkImageSimilarityMetricTLSIterator(
    const typename TLS::iterator& iter) : Pter(0), Iter(iter) {}

  vtkImageSimilarityMetricTLSIterator(T *iter) : Pter(iter) {}

  vtkImageSimilarityMetricTLSIterator& operator++()
    {
    if (this->Pter) { ++this->Pter; } else { ++this->Iter; }
    return *this;
    }

  vtkImageSimilarityMetricTLSIterator operator++(int)
    {
    vtkImageSimilarityMetricTLSIterator copy = *this;
    if (this->Pter) { ++this->Pter; } else { ++this->Iter; }
    return copy;
    }

  bool operator==(const vtkImageSimilarityMetricTLSIterator& other)
    {
    return (this->Pter ? (this->Pter == other.Pter)
                       : (this->Iter == other.Iter));
    }

  bool operator!=(const vtkImageSimilarityMetricTLSIterator& other)
    {
    return (this->Pter ? (this->Pter != other.Pter)
                       : (this->Iter != other.Iter));
    }

  T& operator*()
    {
    return (this->Pter ? *this->Pter : *this->Iter);
    }

  T* operator->()
    {
    return (this->Pter ? this->Pter : &*this->Iter);
    }

private:
  T *Pter;
  typename TLS::iterator Iter;
};

//----------------------------------------------------------------------------
template<typename T>
class vtkImageSimilarityMetricTLS
{
public:
  typedef vtkImageSimilarityMetricTLSIterator<
    T, vtkSMPThreadLocal<T> > iterator;

  vtkImageSimilarityMetricTLS(size_t threads=0)
    {
    if (threads > 0)
      {
      this->SMP = NULL;
      this->MT = new T[threads];
      this->NumberOfThreads = threads;
      }
    else
      {
      this->SMP = new vtkSMPThreadLocal<T>;
      this->MT = NULL;
      this->NumberOfThreads = 0;
      }
    }

  ~vtkImageSimilarityMetricTLS()
    {
    delete this->SMP;
    delete [] this->MT;
    }

  T& Local(size_t threadId)
    {
    if (this->SMP)
      {
      return this->SMP->Local();
      }
    else
      {
      return this->MT[threadId];
      }
    }

  iterator begin()
    {
    if (this->SMP)
      {
      return this->SMP->begin();
      }
    else
      {
      return this->MT;
      }
    }

  iterator end()
    {
    if (this->SMP)
      {
      return this->SMP->end();
      }
    else
      {
      return this->MT + this->NumberOfThreads;
      }
    }

private:
  vtkImageSimilarityMetricTLS(const vtkImageSimilarityMetricTLS&);
  void operator=(const vtkImageSimilarityMetricTLS&);

  vtkSMPThreadLocal<T> *SMP;
  T *MT;
  size_t NumberOfThreads;
};

#else

template<typename T>
class vtkImageSimilarityMetricTLS
{
public:
  typedef T* iterator;

  vtkImageSimilarityMetricTLS(size_t threads)
    {
    this->MT = new T[threads];
    this->NumberOfThreads = threads;
    }

  ~vtkImageSimilarityMetricTLS()
    {
    delete [] this->MT;
    }

  T& Local(size_t threadId)
    {
    return this->MT[threadId];
    }

  iterator begin()
    {
    return this->MT;
    }

  iterator end()
    {
    return this->MT + this->NumberOfThreads;
    }

private:
  vtkImageSimilarityMetricTLS(const vtkImageSimilarityMetricTLS&);
  void operator=(const vtkImageSimilarityMetricTLS&);

  T *MT;
  size_t NumberOfThreads;
};

#endif

//----------------------------------------------------------------------------
// Data needed for each thread.
class vtkImageMutualInformationThreadData
{
public:
  vtkImageMutualInformationThreadData() : Data(0) {}

  vtkIdType *Data;
};

class vtkImageMutualInformationTLS
  : public vtkImageSimilarityMetricTLS<vtkImageMutualInformationThreadData>
{
public:
  vtkImageMutualInformationTLS(size_t threads=0) :
    vtkImageSimilarityMetricTLS<vtkImageMutualInformationThreadData>(threads)
  {}
};

//----------------------------------------------------------------------------
// Constructor sets default values
vtkImageMutualInformation::vtkImageMutualInformation()
{
  this->NumberOfBins[0] = 64;
  this->NumberOfBins[1] = 64;

  this->BinOrigin[0] = 0.0;
  this->BinOrigin[1] = 0.0;

  this->BinSpacing[0] = 1.0;
  this->BinSpacing[1] = 1.0;

  this->OutputScalarType = VTK_FLOAT;

  this->MutualInformation = 0.0;
  this->NormalizedMutualInformation = 0.0;

  this->ThreadData = 0;

  this->SetNumberOfInputPorts(3);
  this->SetNumberOfOutputPorts(1);
}

//----------------------------------------------------------------------------
vtkImageMutualInformation::~vtkImageMutualInformation()
{
}

//----------------------------------------------------------------------------
void vtkImageMutualInformation::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "Stencil: " << this->GetStencil() << "\n";

  os << indent << "NumberOfBins: " << this->NumberOfBins[0] << " "
     << this->NumberOfBins[1] << "\n";
  os << indent << "BinOrigin: " << this->BinOrigin[0] << " "
     << this->BinOrigin[1] << "\n";
  os << indent << "BinSpacing: " << this->BinSpacing[0] << " "
     << this->BinSpacing[1] << "\n";

  os << indent << "MutualInformation: " << this->MutualInformation << "\n";
  os << indent << "NormalizedMutualInformation: "
     << this->NormalizedMutualInformation << "\n";
}

//----------------------------------------------------------------------------
void vtkImageMutualInformation::SetStencilData(vtkImageStencilData *stencil)
{
#if VTK_MAJOR_VERSION >= 6
  this->SetInputData(2, stencil);
#else
  this->SetInput(2, stencil);
#endif
}

//----------------------------------------------------------------------------
vtkImageStencilData *vtkImageMutualInformation::GetStencil()
{
  if (this->GetNumberOfInputConnections(2) < 1)
    {
    return NULL;
    }
  return vtkImageStencilData::SafeDownCast(
    this->GetExecutive()->GetInputData(2, 0));
}

//----------------------------------------------------------------------------
int vtkImageMutualInformation::FillInputPortInformation(int port, vtkInformation *info)
{
  if (port == 0 || port == 1)
    {
    info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkImageData");
    }
  else if (port == 2)
    {
    info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkImageStencilData");
    info->Set(vtkAlgorithm::INPUT_IS_OPTIONAL(), 1);
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkImageMutualInformation::FillOutputPortInformation(int port, vtkInformation* info)
{
  if (port == 0 || port == 1)
    {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkImageData");
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkImageMutualInformation::RequestInformation(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **vtkNotUsed(inputVector),
  vtkInformationVector *outputVector)
{
  int outWholeExt[6];
  double outOrigin[3];
  double outSpacing[3];

  outWholeExt[0] = 0;
  outWholeExt[1] = this->NumberOfBins[0] - 1;
  outWholeExt[2] = 0;
  outWholeExt[3] = this->NumberOfBins[1] - 1;
  outWholeExt[4] = 0;
  outWholeExt[5] = 0;

  outOrigin[0] = this->BinOrigin[0];
  outOrigin[1] = this->BinOrigin[1];
  outOrigin[2] = 0.0;

  outSpacing[0] = this->BinSpacing[0];
  outSpacing[1] = this->BinSpacing[1];
  outSpacing[2] = 1.0;

  int outScalarType = this->OutputScalarType;
  int outComponents = 1;

  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),outWholeExt,6);
  outInfo->Set(vtkDataObject::ORIGIN(), outOrigin, 3);
  outInfo->Set(vtkDataObject::SPACING(), outSpacing, 3);

  vtkDataObject::SetPointDataActiveScalarInfo(
      outInfo, outScalarType, outComponents);

  return 1;
}

//----------------------------------------------------------------------------
int vtkImageMutualInformation::RequestUpdateExtent(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *vtkNotUsed(outputVector))
{
  int inExt0[6], inExt1[6];
  vtkInformation *inInfo0 = inputVector[0]->GetInformationObject(0);
  vtkInformation *inInfo1 = inputVector[1]->GetInformationObject(0);

  inInfo0->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), inExt0);
  inInfo1->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), inExt1);

  inInfo0->Set(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(), inExt0, 6);
  inInfo1->Set(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(), inExt1, 6);

  // need to set the stencil update extent to the input extent
  if (this->GetNumberOfInputConnections(2) > 0)
    {
    vtkInformation *stencilInfo = inputVector[2]->GetInformationObject(0);
    stencilInfo->Set(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
                     inExt0, 6);
    }

  return 1;
}

//----------------------------------------------------------------------------
// anonymous namespace for internal classes and functions
namespace {

struct vtkImageMutualInformationThreadStruct
{
  vtkImageMutualInformation *Algorithm;
  vtkInformation *Request;
  vtkInformationVector **InputsInfo;
  vtkInformationVector *OutputsInfo;
};

//----------------------------------------------------------------------------
// override from vtkThreadedImageAlgorithm to split input extent, instead
// of splitting the output extent
VTK_THREAD_RETURN_TYPE vtkImageMutualInformationThreadedExecute(void *arg)
{
  vtkMultiThreader::ThreadInfo *ti =
    static_cast<vtkMultiThreader::ThreadInfo *>(arg);
  vtkImageMutualInformationThreadStruct *ts =
    static_cast<vtkImageMutualInformationThreadStruct *>(ti->UserData);

  int extent[6] = { 0, -1, 0, -1, 0, -1 };

  bool foundConnection = false;
  int numPorts = ts->Algorithm->GetNumberOfInputPorts();
  for (int inPort = 0; inPort < numPorts; ++inPort)
    {
    int numConnections = ts->Algorithm->GetNumberOfInputConnections(inPort);
    if (numConnections)
      {
      vtkInformation *inInfo =
        ts->InputsInfo[inPort]->GetInformationObject(0);
      vtkImageData *inData = vtkImageData::SafeDownCast(
        inInfo->Get(vtkDataObject::DATA_OBJECT()));
      if (inData)
        {
        inData->GetExtent(extent);
        foundConnection = true;
        break;
        }
      }
    }

  if (foundConnection)
    {
    // execute the actual method with appropriate extent
    // first find out how many pieces extent can be split into.
    int splitExt[6];
    int total = ts->Algorithm->SplitExtent(
      splitExt, extent, ti->ThreadID, ti->NumberOfThreads);

    if (ti->ThreadID < total &&
        splitExt[1] >= splitExt[0] &&
        splitExt[3] >= splitExt[2] &&
        splitExt[5] >= splitExt[4])
      {
      ts->Algorithm->ThreadedRequestData(
        ts->Request, ts->InputsInfo, ts->OutputsInfo, NULL, NULL,
        splitExt, ti->ThreadID);
      }
    }

  return VTK_THREAD_RETURN_VALUE;
}

//----------------------------------------------------------------------------
template<class T1, class T2>
void vtkImageMutualInformationExecute(
  vtkImageMutualInformation *self,
  vtkImageData *inData0, vtkImageData *inData1, vtkImageStencilData *stencil,
  T1 *inPtr, T2 *inPtr1, int extent[6],
  vtkIdType *outPtr, int numBins[2], double binOrigin[2], double binSpacing[2],
  int threadId)
{
  vtkImageStencilIterator<T1>
    inIter(inData0, stencil, extent, ((threadId == 0) ? self : NULL));
  vtkImageStencilIterator<T2>
    inIter1(inData1, stencil, extent, NULL);

  int pixelInc = inData0->GetNumberOfScalarComponents();
  int pixelInc1 = inData1->GetNumberOfScalarComponents();

  static double xmin = 0;
  static double ymin = 0;
  double xmax = numBins[0] - 1;
  double ymax = numBins[1] - 1;
  double xshift = -binOrigin[0];
  double yshift = -binOrigin[1];
  double xscale = 1.0/binSpacing[0];
  double yscale = 1.0/binSpacing[1];
  int outIncY = numBins[1];

  // iterate over all spans in the stencil
  while (!inIter.IsAtEnd())
    {
    if (inIter.IsInStencil())
      {
      inPtr = inIter.BeginSpan();
      T1 *inPtrEnd = inIter.EndSpan();
      inPtr1 = inIter1.BeginSpan();

      // iterate over all voxels in the span
      while (inPtr != inPtrEnd)
        {
        double x = *inPtr;
        double y = *inPtr1;

        x += xshift;
        x *= xscale;

        y += yshift;
        y *= yscale;

        x = (x > xmin ? x : xmin);
        x = (x < xmax ? x : xmax);

        y = (y > ymin ? y : ymin);
        y = (y < ymax ? y : ymax);

        int xi = static_cast<int>(x + 0.5);
        int yi = static_cast<int>(y + 0.5);

        vtkIdType *outPtr1 = outPtr + yi*outIncY + xi;

        (*outPtr1)++;

        inPtr += pixelInc;
        inPtr1 += pixelInc1;
        }
      }
    inIter.NextSpan();
    inIter1.NextSpan();
    }
}

//----------------------------------------------------------------------------
void vtkImageMutualInformationExecutePreScaled(
  vtkImageMutualInformation *self,
  vtkImageData *inData0, vtkImageData *inData1, vtkImageStencilData *stencil,
  unsigned char *inPtr, unsigned char *inPtr1, int extent[6],
  vtkIdType *outPtr, int numBins[2], int threadId)
{
  vtkImageStencilIterator<unsigned char>
    inIter(inData0, stencil, extent, ((threadId == 0) ? self : NULL));

  vtkImageStencilIterator<unsigned char>
    inIter1(inData1, stencil, extent, NULL);

  int pixelInc = inData0->GetNumberOfScalarComponents();
  int pixelInc1 = inData1->GetNumberOfScalarComponents();

  int xmax = numBins[0] - 1;
  int ymax = numBins[1] - 1;
  int outIncY = numBins[1];

  // iterate over all spans in the stencil
  while (!inIter.IsAtEnd())
    {
    if (inIter.IsInStencil())
      {
      inPtr = inIter.BeginSpan();
      unsigned char *inPtrEnd = inIter.EndSpan();
      inPtr1 = inIter1.BeginSpan();

      // iterate over all voxels in the span
      while (inPtr != inPtrEnd)
        {
        int x = *inPtr;
        int y = *inPtr1;

        x = (x < xmax ? x : xmax);
        y = (y < ymax ? y : ymax);

        vtkIdType *outPtr1 = outPtr + y*outIncY + x;

        (*outPtr1)++;

        inPtr += pixelInc;
        inPtr1 += pixelInc1;
        }
      }
    inIter.NextSpan();
    inIter1.NextSpan();
    }
}

//----------------------------------------------------------------------------
// copy one row of the joint histogram to the output, with conversion
// but without type range checking
template<class T>
void vtkImageMutualInformationCopyRow(
  vtkIdType *xyHist, T *outPtr, int outStart, int outEnd)
{
  int n = outEnd - outStart + 1;
  xyHist += outStart;

  do
    {
    *outPtr++ = static_cast<T>(*xyHist++);
    }
  while (--n);
}



} // end anonymous namespace

//----------------------------------------------------------------------------
#ifdef USE_SMP_THREADED_IMAGE_ALGORITHM
// Functor for vtkSMPTools execution
class vtkImageMutualInformationFunctor
{
public:
  // Create the functor, provide all info it needs to execute.
  vtkImageMutualInformationFunctor(
    vtkImageMutualInformationThreadStruct *pipelineInfo,
    const int extent[6],
    vtkIdType pieces)
    : PipelineInfo(pipelineInfo),
      NumberOfPieces(pieces)
  {
    for (int i = 0; i < 6; i++)
      {
      this->Extent[i] = extent[i];
      }
  }

  void Initialize() {}
  void operator()(vtkIdType begin, vtkIdType end);
  void Reduce();

private:
  vtkImageMutualInformationThreadStruct *PipelineInfo;
  int Extent[6];
  vtkIdType NumberOfPieces;
};

// Called by vtkSMPTools to execute the algorithm over specific pieces.
void vtkImageMutualInformationFunctor::operator()(
  vtkIdType begin, vtkIdType end)
{
  vtkImageMutualInformationThreadStruct *ts = this->PipelineInfo;

  ts->Algorithm->SMPRequestData(
    ts->Request, ts->InputsInfo, ts->OutputsInfo, NULL, NULL,
    begin, end, this->NumberOfPieces, this->Extent);
}

// Called by vtkSMPTools once the multi-threading has finished.
void vtkImageMutualInformationFunctor::Reduce()
{
  vtkImageMutualInformation *self =
    static_cast<vtkImageMutualInformation *>(this->PipelineInfo->Algorithm);
  vtkImageMutualInformationThreadStruct *ts = this->PipelineInfo;

  self->ReduceRequestData(ts->Request, ts->InputsInfo, ts->OutputsInfo);
}
#endif

//----------------------------------------------------------------------------
// override from vtkThreadedImageAlgorithm to customize the multithreading
int vtkImageMutualInformation::RequestData(
  vtkInformation* request,
  vtkInformationVector** inputVector,
  vtkInformationVector* outputVector)
{
  // start of code copied from vtkThreadedImageAlgorithm

  // setup the threads structure
  vtkImageMutualInformationThreadStruct ts;
  ts.Algorithm = this;
  ts.Request = request;
  ts.InputsInfo = inputVector;
  ts.OutputsInfo = outputVector;

  // allocate the output data
  int numberOfOutputs = this->GetNumberOfOutputPorts();
  if (numberOfOutputs > 0)
    {
    for (int i = 0; i < numberOfOutputs; ++i)
      {
      vtkInformation* info = outputVector->GetInformationObject(i);
      vtkImageData *outData = vtkImageData::SafeDownCast(
        info->Get(vtkDataObject::DATA_OBJECT()));
      if (outData)
        {
        int updateExtent[6];
        info->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
                  updateExtent);
#if VTK_MAJOR_VERSION >= 6
        this->AllocateOutputData(outData, info, updateExtent);
#else
        this->AllocateOutputData(outData, updateExtent);
#endif
        }
      }
    }

  // copy arrays from first input to output
  int numberOfInputs = this->GetNumberOfInputPorts();
  if (numberOfInputs > 0)
    {
    vtkInformationVector* portInfo = inputVector[0];
    int numberOfConnections = portInfo->GetNumberOfInformationObjects();
    if (numberOfConnections && numberOfOutputs)
      {
      vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
      vtkImageData *inData = vtkImageData::SafeDownCast(
        inInfo->Get(vtkDataObject::DATA_OBJECT()));
      vtkInformation* outInfo = outputVector->GetInformationObject(0);
      vtkImageData *outData = vtkImageData::SafeDownCast(
        outInfo->Get(vtkDataObject::DATA_OBJECT()));
      this->CopyAttributeData(inData, outData, inputVector);
      }
    }

  // end of code copied from vtkThreadedImageAlgorithm

#ifdef USE_SMP_THREADED_IMAGE_ALGORITHM
  if (this->EnableSMP)
    {
    // code for vtkSMPTools
    vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
    vtkImageData *inData = vtkImageData::SafeDownCast(
      inInfo->Get(vtkDataObject::DATA_OBJECT()));
    int extent[6];
    inData->GetExtent(extent);

    // do a dummy execution of SplitExtent to compute the number of pieces
    vtkIdType pieces = this->SplitExtent(0, extent, 0, this->NumberOfThreads);

    // create the thread-local object and the functor
    vtkImageMutualInformationTLS tlocal;
    vtkImageMutualInformationFunctor functor(&ts, extent, pieces);

    this->ThreadData = &tlocal;
    bool debug = this->Debug;
    this->Debug = false;
    vtkSMPTools::For(0, pieces, functor);
    this->Debug = debug;
    this->ThreadData = 0;
    }
  else
#endif
    {
    // code for vtkMultiThreader
    vtkImageMutualInformationTLS tlocal(this->NumberOfThreads);
    this->ThreadData = &tlocal;
    this->Threader->SetNumberOfThreads(this->NumberOfThreads);
    this->Threader->SetSingleMethod(
      vtkImageMutualInformationThreadedExecute, &ts);

    // always shut off debugging to avoid threading problems with GetMacros
    int debug = this->Debug;
    this->Debug = 0;
    this->Threader->SingleMethodExecute();
    this->Debug = debug;

    this->ReduceRequestData(request, inputVector, outputVector);

    this->ThreadData = 0;
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkImageMutualInformation::ReduceRequestData(
  vtkInformation *, vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  // get the output
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  int updateExtent[6];
  outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
               updateExtent);
  vtkImageData *outData = vtkImageData::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  // get the output pointer
  void *outPtr = outData->GetScalarPointerForExtent(updateExtent);
  int outScalarType = outData->GetScalarType();
  int outScalarSize = outData->GetScalarSize();

  // get the dimensions of the joint histogram
  int nx = this->NumberOfBins[0];
  int ny = this->NumberOfBins[1];

  // various variables for computing the mutual information
  double xEntropy = 0;
  double yEntropy = 0;
  double xyEntropy = 0;

  // allocate space to accumulate results
  vtkIdType *xyHist = new vtkIdType[2*nx + ny];
  vtkIdType *xHist = xyHist + nx;

  // clear xHist to zero
  int ix;
  for (ix = 0; ix < nx; ++ix)
    {
    xHist[ix] = 0;
    }

  // piece together the joint histogram results from each thread
  for (int iy = 0; iy < ny; ++iy)
    {
    int outStart = updateExtent[0];
    int outEnd = updateExtent[1];
    if (iy < updateExtent[2] || iy > updateExtent[3])
      {
      outStart = 0;
      outEnd = -1;
      }

    // clear the output
    for (ix = 0; ix < nx; ++ix)
      {
      xyHist[ix] = 0;
      }

    // add the contribution from thread j
    vtkIdType a = 0;
    for (vtkImageMutualInformationTLS::iterator
         iter = this->ThreadData->begin();
         iter != this->ThreadData->end(); ++iter)
      {
      if (iter->Data)
        {
        vtkIdType *outPtr2 = iter->Data + static_cast<vtkIdType>(nx)*iy;

        for (ix = 0; ix < nx; ++ix)
          {
          vtkIdType c = *outPtr2++;
          xyHist[ix] += c;
          a += c;
          }
        }
      }

    // copy this row of the joint histogram to the output
    if (outStart <= outEnd)
      {
      switch (outScalarType)
        {
        vtkTemplateAliasMacro(
          vtkImageMutualInformationCopyRow(
            xyHist, static_cast<VTK_TT *>(outPtr), outStart, outEnd));
        default:
          vtkErrorMacro("Execute: Unknown output ScalarType");
        }
      // increment outPtr to the next row
      outPtr =
        static_cast<char *>(outPtr) + outScalarSize*(outEnd - outStart + 1);
      }

    // compute the entropy of second image
    double da = static_cast<double>(a);
    if (da > 0)
      {
      yEntropy += da*log(da);
      }

    // compute joint entropy
    for (ix = 0; ix < nx; ++ix)
      {
      vtkIdType c = xyHist[ix];
      xHist[ix] += c;
      double dc = static_cast<double>(c);
      if (dc > 0)
        {
        xyEntropy += dc*log(dc);
        }
      }
    }

  // compute total pixel count and entropy of first image
  vtkIdType count = 0;
  for (ix = 0; ix < nx; ++ix)
    {
    vtkIdType b = xHist[ix];
    count += b;
    double db = static_cast<double>(b);
    if (db > 0)
      {
      xEntropy += db*log(db);
      }
    }

  for (vtkImageMutualInformationTLS::iterator
       iter = this->ThreadData->begin();
       iter != this->ThreadData->end(); ++iter)
    {
    // delete the temporary memory
    delete [] iter->Data;
    }

  delete [] xyHist;

  // minimum possible values
  double mutualInformation = 0.0;
  double normalizedMutualInformation = 1.0;

  if (count)
    {
    // correct for total voxel count, convert to negative
    double dc = static_cast<double>(count);
    double ldc = log(dc);
    xEntropy = -xEntropy/dc + ldc;
    yEntropy = -yEntropy/dc + ldc;
    xyEntropy = -xyEntropy/dc + ldc;

    // compute the mutual information
    mutualInformation = xEntropy + yEntropy - xyEntropy;

    // compute the normalized mutual information (Studholme 1999)
    normalizedMutualInformation = (xEntropy + yEntropy)/xyEntropy;
    }

  this->MutualInformation = mutualInformation;
  this->NormalizedMutualInformation = normalizedMutualInformation;

  return 1;
}

//----------------------------------------------------------------------------
// turn off 64-bit ints when templating over all types
# undef VTK_USE_INT64
# define VTK_USE_INT64 0
# undef VTK_USE_UINT64
# define VTK_USE_UINT64 0

namespace {

//----------------------------------------------------------------------------
template<class T1>
void vtkImageMutualInformationExecute1(
  vtkImageMutualInformation *self,
  vtkImageData *inData0, vtkImageData *inData1, vtkImageStencilData *stencil,
  T1 *inPtr, void *inPtr1, int extent[6],
  vtkIdType *outPtr, int numBins[2], double binOrigin[2], double binSpacing[2],
  int threadId)
{
  switch (inData1->GetScalarType())
    {
    vtkTemplateAliasMacro(
      vtkImageMutualInformationExecute(
        self, inData0, inData1, stencil,
        inPtr, static_cast<VTK_TT *>(inPtr1), extent,
        outPtr, numBins, binOrigin, binSpacing, threadId));
    default:
      vtkErrorWithObjectMacro(self, "Execute: Unknown input ScalarType");
    }
}

} // end anonymous namespace

//----------------------------------------------------------------------------
// This method is passed a input and output region, and executes the filter
// algorithm to fill the output from the input.
// It just executes a switch statement to call the correct function for
// the regions data types.
void vtkImageMutualInformation::ThreadedRequestData(
  vtkInformation *vtkNotUsed(request),
  vtkInformationVector **inputVector,
  vtkInformationVector *vtkNotUsed(outputVector),
  vtkImageData ***vtkNotUsed(inData),
  vtkImageData **vtkNotUsed(outData),
  int extent[6], int threadId)
{
  vtkImageMutualInformationThreadData *threadLocal =
    &this->ThreadData->Local(threadId);

  vtkIdType *outPtr = threadLocal->Data;

  if (outPtr == 0)
    {
    // initialize the joint histogram to zero
    vtkIdType outIncY = this->NumberOfBins[0];
    vtkIdType outCount = outIncY;
    outCount *= this->NumberOfBins[1];

    threadLocal->Data = new vtkIdType[outCount];
    outPtr = threadLocal->Data;

    vtkIdType *outPtr1 = outPtr;
    do { *outPtr1++ = 0; } while (--outCount > 0);
    }

  vtkInformation *inInfo0 = inputVector[0]->GetInformationObject(0);
  vtkInformation *inInfo1 = inputVector[1]->GetInformationObject(0);

  vtkImageData *inData0 = vtkImageData::SafeDownCast(
    inInfo0->Get(vtkDataObject::DATA_OBJECT()));
  vtkImageData *inData1 = vtkImageData::SafeDownCast(
    inInfo1->Get(vtkDataObject::DATA_OBJECT()));

  // make sure execute extent is not beyond the extent of any input
  int inExt0[6], inExt1[6];
  inData0->GetExtent(inExt0);
  inData1->GetExtent(inExt1);

  for (int i = 0; i < 6; i += 2)
    {
    int j = i + 1;
    extent[i] = ((extent[i] > inExt0[i]) ? extent[i] : inExt0[i]);
    extent[i] = ((extent[i] > inExt1[i]) ? extent[i] : inExt1[i]);
    extent[j] = ((extent[j] < inExt0[j]) ? extent[j] : inExt0[j]);
    extent[j] = ((extent[j] < inExt1[j]) ? extent[j] : inExt1[j]);
    if (extent[i] > extent[j])
      {
      return;
      }
    }

  void *inPtr0 = inData0->GetScalarPointerForExtent(extent);
  void *inPtr1 = inData1->GetScalarPointerForExtent(extent);

  vtkImageStencilData *stencil = this->GetStencil();

  double *binOrigin = this->BinOrigin;
  double *binSpacing = this->BinSpacing;
  int *numBins = this->NumberOfBins;
  int maxX = numBins[0] - 1;
  int maxY = numBins[1] - 1;

  if (vtkMath::Floor(binOrigin[0] + 0.5) == 0 &&
      vtkMath::Floor(binOrigin[1] + 0.5) == 0 &&
      vtkMath::Floor(binOrigin[0] + binSpacing[0]*maxX + 0.5) == maxX &&
      vtkMath::Floor(binOrigin[1] + binSpacing[1]*maxY + 0.5) == maxY &&
      inData0->GetScalarType() == VTK_UNSIGNED_CHAR &&
      inData1->GetScalarType() == VTK_UNSIGNED_CHAR)
    {
    vtkImageMutualInformationExecutePreScaled(
      this, inData0, inData1, stencil,
      static_cast<unsigned char *>(inPtr0),
      static_cast<unsigned char *>(inPtr1),
      extent, outPtr, numBins, threadId);
    }
  else switch (inData0->GetScalarType())
    {
    vtkTemplateAliasMacro(
      vtkImageMutualInformationExecute1(
        this, inData0, inData1, stencil,
        static_cast<VTK_TT *>(inPtr0), inPtr1,
        extent, outPtr, numBins, binOrigin, binSpacing,
        threadId));
    default:
      vtkErrorMacro(<< "Execute: Unknown ScalarType");
    }
}
