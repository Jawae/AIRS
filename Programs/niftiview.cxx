#include "vtkNIFTIReader.h"

#include "vtkRenderWindowInteractor.h"
#include "vtkInteractorStyleImage.h"
#include "vtkImageSincInterpolator.h"
#include "vtkRenderWindow.h"
#include "vtkRenderer.h"
#include "vtkActor.h"
#include "vtkProperty.h"
#include "vtkCamera.h"
#include "vtkCommand.h"
#include "vtkLineSource.h"
#include "vtkMath.h"
#include "vtkMatrix4x4.h"
#include "vtkImageData.h"
#include "vtkImageReslice.h"
#include "vtkImageResliceMapper.h"
#include "vtkImageProperty.h"
#include "vtkImageSlice.h"
#include "vtkPlane.h"
#include "vtkPolyDataMapper.h"
#include "vtkSmartPointer.h"
#include "vtkStringArray.h"

class SliceObserver : public vtkCommand
{
public:
  static SliceObserver *New() { return new SliceObserver; }
  vtkTypeMacro(SliceObserver, vtkCommand);
  virtual void Execute(vtkObject *, unsigned long, void *);

  vtkSmartPointer<vtkImageResliceMapper> Mapper;
  vtkSmartPointer<vtkLineSource> Line[2];
  vtkSmartPointer<vtkImageData> Image;

protected:
  SliceObserver() {}
  ~SliceObserver() {}
};

void SliceObserver::Execute(vtkObject *o, unsigned long, void *)
{
  double position[3];
  vtkCamera *camera = vtkCamera::SafeDownCast(o);
  camera->GetFocalPoint(position);

  if (this->Image.GetPointer() != 0)
    {
    double origin[3], spacing[3];
    this->Image->GetOrigin(origin);
    this->Image->GetSpacing(spacing);
    int s = vtkMath::Floor((position[2] - origin[2])/spacing[2] + 0.5);
    position[2] = s*spacing[2] + origin[2];
    }

  for (int i = 0; i < 2; i++)
    {
    if (this->Line[i].GetPointer() != 0)
      {
      double p[3];
      this->Line[i]->GetPoint1(p);
      p[2] = position[2];
      this->Line[i]->SetPoint1(p);

      this->Line[i]->GetPoint2(p);
      p[2] = position[2];
      this->Line[i]->SetPoint2(p);
      }
    }
}

int main(int argc, char *argv[])
{
  vtkSmartPointer<SliceObserver> sliceObserver =
    vtkSmartPointer<SliceObserver>::New();
  vtkSmartPointer<vtkRenderWindowInteractor> iren =
    vtkSmartPointer<vtkRenderWindowInteractor>::New();
  vtkSmartPointer<vtkInteractorStyleImage> style =
    vtkSmartPointer<vtkInteractorStyleImage>::New();
  style->SetInteractionModeToImageSlicing();
  vtkSmartPointer<vtkRenderWindow> renWin =
    vtkSmartPointer<vtkRenderWindow>::New();
  iren->SetRenderWindow(renWin);
  iren->SetInteractorStyle(style);

  bool interp = 1;
  const char *filename = 0;
  if (argc > 1)
    {
    filename = argv[1];
    if (argc > 2 && strcmp(argv[1], "--nointerp") == 0)
      {
      interp = 0;
      filename = argv[2];
      }
    }
  else
    {
    fprintf(stderr, "usage: niftiview [--nointerp] <filename>\n");
    return 0;
    }

  vtkSmartPointer<vtkNIFTIReader> reader =
    vtkSmartPointer<vtkNIFTIReader>::New();
  reader->SetFileName(filename);
  reader->Update();

  vtkSmartPointer<vtkMatrix4x4> matrix =
    vtkSmartPointer<vtkMatrix4x4>::New();
  if (reader->GetQFormMatrix())
    {
    matrix->DeepCopy(reader->GetQFormMatrix());
    matrix->Invert();
    }
  else if (reader->GetSFormMatrix())
    {
    matrix->DeepCopy(reader->GetSFormMatrix());
    matrix->Invert();
    }

  vtkSmartPointer<vtkImageReslice> reslice =
    vtkSmartPointer<vtkImageReslice>::New();
  reslice->SetInputConnection(reader->GetOutputPort());
  reslice->SetResliceAxes(matrix);
  reslice->SetInterpolationModeToLinear();
  reslice->Update();

  double range[2];
  int extent[6];
  reslice->GetOutput()->GetScalarRange(range);
  reslice->GetOutput()->GetExtent(extent);

  static double viewport[3][4] = {
    { 0.5, 0.0, 1.0, 0.5 },
    { 0.5, 0.5, 1.0, 1.0 },
    { 0.0, 0.0, 0.5, 1.0 },
  };

  // check if image is 2D
  bool imageIs3D = (extent[5] > extent[4]);

  for (int i = 2*(imageIs3D == 0); i < 3; i++)
    {
    vtkSmartPointer<vtkImageResliceMapper> imageMapper =
      vtkSmartPointer<vtkImageResliceMapper>::New();
    imageMapper->SetInputConnection(reslice->GetOutputPort());
    imageMapper->SliceFacesCameraOn();
    imageMapper->SliceAtFocalPointOn();
    imageMapper->BorderOn();
    if (interp)
      {
      vtkSmartPointer<vtkImageSincInterpolator> sincInterpolator =
        vtkSmartPointer<vtkImageSincInterpolator>::New();
      sincInterpolator->SetWindowFunctionToBlackman();
      sincInterpolator->AntialiasingOn();
      imageMapper->SetInterpolator(sincInterpolator);
      imageMapper->JumpToNearestSliceOn();
      imageMapper->ResampleToScreenPixelsOn();
      imageMapper->AutoAdjustImageQualityOff();
      }

    vtkSmartPointer<vtkImageSlice> image =
      vtkSmartPointer<vtkImageSlice>::New();
    image->SetMapper(imageMapper);

    image->GetProperty()->SetColorWindow(range[1] - range[0]);
    image->GetProperty()->SetColorLevel(0.5*(range[0] + range[1]));
    if (!interp)
      {
      image->GetProperty()->SetInterpolationTypeToNearest();
      }

    vtkSmartPointer<vtkRenderer> renderer =
      vtkSmartPointer<vtkRenderer>::New();
    renderer->AddViewProp(image);
    renderer->SetBackground(0.0, 0.0, 0.0);
    if (imageIs3D)
      {
      renderer->SetViewport(viewport[i]);
      }

    renWin->AddRenderer(renderer);

    // use center point to set camera
    double *bounds = imageMapper->GetBounds();
    double point[3];
    point[0] = 0.5*(bounds[0] + bounds[1]);
    point[1] = 0.5*(bounds[2] + bounds[3]);
    point[2] = 0.5*(bounds[4] + bounds[5]);
    double maxdim = 0.0;
    for (int j = 0; j < 3; j++)
      {
      double s = 0.5*(bounds[2*j+1] - bounds[2*j]);
      maxdim = (s > maxdim ? s : maxdim);
      }

    vtkCamera *camera = renderer->GetActiveCamera();
    camera->SetFocalPoint(point);
    if (i == 2)
      {
      point[2] -= 500.0;
      camera->SetViewUp(0.0, -1.0, 0.0);
      camera->SetParallelScale(maxdim);
      camera->AddObserver(vtkCommand::ModifiedEvent, sliceObserver);
      sliceObserver->Image = reslice->GetOutput();
      }
    else
      {
      vtkSmartPointer<vtkLineSource> lineSource =
        vtkSmartPointer<vtkLineSource>::New();
      double p[3] = { point[0], point[1], point[2] };
      p[i] += 10;
      p[1 - i] = bounds[2*(1 - i)];
      lineSource->SetPoint1(p);
      p[1 - i] = bounds[2*(1 - i) + 1];
      lineSource->SetPoint2(p);

      vtkSmartPointer<vtkPolyDataMapper> lineMapper =
        vtkSmartPointer<vtkPolyDataMapper>::New();
      lineMapper->SetInputConnection(lineSource->GetOutputPort());

      vtkSmartPointer<vtkActor> lineActor =
        vtkSmartPointer<vtkActor>::New();
      lineActor->SetMapper(lineMapper);
      lineActor->GetProperty()->SetColor(1.0, 0.0, 0.0);

      renderer->AddActor(lineActor);

      sliceObserver->Line[i] = lineSource;

      point[i] += 500.0;
      camera->SetViewUp(0.0, 0.0, +1.0);
      camera->SetParallelScale(maxdim/2);
      }
    camera->SetPosition(point);
    camera->ParallelProjectionOn();
    }

  int width = extent[1] - extent[0] + 1;
  int height = extent[3] - extent[2] + 1;

  if (imageIs3D)
    {
    renWin->SetSize(2*width, height);
    }
  else
    {
    renWin->SetSize(width, height);
    }

  renWin->Render();
  iren->Start();

  return 0;
}
