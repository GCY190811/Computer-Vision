#include "ped_train.hpp"

#include <getopt.h>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <LibSVM/svm.h>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/ml/ml.hpp>
#include <opencv2/objdetect/objdetect.hpp>  // HOGDescriptor

#define Malloc(type, n) (type *)malloc((n) * sizeof(type))

using namespace std;
using namespace cv;
using namespace ml;

#define KERNEL (SVM::RBF)
#define verbose (True)
#define SVM_C (10)
#define GAMMA (0.001)

// svm parameters
struct svm_parameter param;
struct svm_problem prob;
svm_model *model;
struct svm_node *x_space;
int cross_validation;
int nr_fold;

// args
#define BUFFER_LEN (256)
char pos_image_path[BUFFER_LEN];
char neg_image_path[BUFFER_LEN];
char model_params[BUFFER_LEN];
char model_file_name[BUFFER_LEN];

void ZScore(cv::Mat &trainData, vector<double> &mean, vector<double> &standard);

void prepare_param();
void saveParams(vector<double> &mean, vector<double> &standard);

void arg_parse(int argc, char **argv);

int main(int argc, char **argv) {
  arg_parse(argc, argv);

  // Image file
  cv::String PedPath = string(pos_image_path) + "/*";
  cv::String NotPedPath = string(neg_image_path) + "/*";

  vector<cv::String> pedFn;
  vector<cv::String> NotPedFn;

  cv::glob(PedPath, pedFn, false);
  cv::glob(NotPedPath, NotPedFn, false);

  cout << "pos files: " << pedFn.size() << endl;
  cout << "neg files: " << NotPedFn.size() << endl;

  // cvHog
  cv::HOGDescriptor cvHog(
      Size(IMAGE_WIDTH, IMAGE_HEIGHT),
      Size(PIXEL_PER_CELL * CELL_PER_BLOCK, PIXEL_PER_CELL * CELL_PER_BLOCK),
      Size(PIXEL_PER_CELL, PIXEL_PER_CELL),
      Size(PIXEL_PER_CELL, PIXEL_PER_CELL), ORIENT, 1, (-1.0),
      HOGDescriptor::L2Hys, 0.2, true);

  int DescriptorDim = ORIENT * CELL_PER_BLOCK * CELL_PER_BLOCK *
                      (IMAGE_WIDTH / PIXEL_PER_CELL - 1) *
                      (IMAGE_HEIGHT / PIXEL_PER_CELL - 1);

  int train_image_nums = pedFn.size() + NotPedFn.size();
  cv::Mat trainFeatureMat = cv::Mat::zeros(train_image_nums, DescriptorDim,
                                           CV_32FC1);  // Data for cv::Mat
  cv::Mat trainLabelMat = cv::Mat::zeros(train_image_nums, 1,
                                         CV_32SC1);  // row: data num, col=1
  vector<int> order(train_image_nums);

  // pos Label = 1
  for (size_t i = 0; i < pedFn.size(); i++) {
    cv::Mat image = imread(pedFn[i]);

    vector<float> descriptors;
    cvHog.compute(image, descriptors);

    for (int j = 0; j < DescriptorDim; j++) {
      trainFeatureMat.at<float>(i, j) = descriptors[j];
    }

    trainLabelMat.at<int>(i, 0) = 1;
    order[i] = i;
  }

  // neg Label = 0
  for (size_t i = 0; i < NotPedFn.size(); i++) {
    cv::Mat image = imread(NotPedFn[i]);

    vector<float> descriptors;
    cvHog.compute(image, descriptors);

    for (size_t j = 0; j < DescriptorDim; j++) {
      trainFeatureMat.at<float>(i + pedFn.size(), j) = descriptors[j];
    }

    trainLabelMat.at<int>(i + pedFn.size(), 0) = 0;
    order[pedFn.size() + i] = pedFn.size() + i;
  }

// shuffle
#if TRAINMODEL
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
#else
  unsigned seed = 7;
#endif

  shuffle(order.begin(), order.end(), std::default_random_engine(seed));

  vector<double> mean(DescriptorDim, 0);
  vector<double> standard(DescriptorDim, 1);
  ZScore(trainFeatureMat, mean, standard);

  // save mean/std or test order num
  saveParams(mean, standard);

  // svm params
  prepare_param();

  prob.l = trainFeatureMat.rows;
  prob.y = Malloc(double, prob.l);
  prob.x = Malloc(struct svm_node *, prob.l);
  x_space = Malloc(struct svm_node, (trainFeatureMat.cols + 1) * prob.l);

  for (size_t i = 0; i < trainFeatureMat.rows; i++) {
    prob.x[i] = &x_space[i * (trainFeatureMat.cols + 1)];
    prob.y[i] = (double)trainLabelMat.at<int>(order[i], 0);
    size_t j = 0;
    for (; j < trainFeatureMat.cols; j++) {
      x_space[i * (trainFeatureMat.cols + 1) + j].index = j + 1;
      x_space[i * (trainFeatureMat.cols + 1) + j].value =
          (double)trainFeatureMat.at<float>(order[i], j);
    }
    x_space[i * (trainFeatureMat.cols + 1) + j].index = -1;
  }

  const char *error_msg;
  error_msg = svm_check_parameter(&prob, &param);
  if (error_msg) {
    printf("ERROR: %s\n", error_msg);
    return -1;
  }

  model = svm_train(&prob, &param);
  if (svm_save_model(model_file_name, model)) {
    printf("can't save model to file %s\n", model_file_name);
    return -2;
  }
  svm_free_and_destroy_model(&model);
  svm_destroy_param(&param);
  free(prob.y);
  free(prob.x);
  free(x_space);

  return 0;
}

void arg_parse(int argc, char **argv) {
  int opt;
  char *arg_str = "p:n:m:r:";

  while ((opt = getopt(argc, argv, arg_str)) != -1) {
    switch (opt) {
      case 'p':
        snprintf(pos_image_path, BUFFER_LEN, "%s", optarg);
        printf("-p: %s\n", pos_image_path);
        break;
      case 'n':
        snprintf(neg_image_path, BUFFER_LEN, "%s", optarg);
        printf("-n: %s\n", neg_image_path);
        break;
      case 'm':
        snprintf(model_file_name, BUFFER_LEN, "%s", optarg);
        printf("-m: %s\n", model_file_name);
        break;
      case 'r':
        snprintf(model_params, BUFFER_LEN, "%s", optarg);
        printf("-param(r): %s\n", model_params);
        break;
      default:
        break;
    }
  }
}

void ZScore(cv::Mat &trainData, vector<double> &mean,
            vector<double> &standard) {
  for (size_t i = 0; i < trainData.cols; i++) {
    cv::Mat meanCol, stdCol;
    cv::Mat column = cv::Mat::zeros(trainData.rows, 1, CV_32FC1);
    for (size_t j = 0; j < trainData.rows; j++) {
      column.at<float>(j, 0) = trainData.at<float>(j, i);
    }
    cv::meanStdDev(column, meanCol, stdCol);
    mean[i] = meanCol.at<double>(0, 0);
    standard[i] = stdCol.at<double>(0, 0);
    for (size_t j = 0; j < trainData.rows; j++) {
      trainData.at<float>(j, i) =
          float((column.at<float>(j, 0) - mean[i]) / (standard[i] + 1e-5));
    }
  }
}

void saveParams(vector<double> &mean, vector<double> &standard) {
  // unsigned seed =
  // std::chrono::system_clock::now().time_since_epoch().count();
  // shuffle(order.begin(), order.end(), std::default_random_engine(seed));

  // int startnum = (int) order.size()*0.9;

  // ofstream ofiletrain;
  // ofiletrain.open("/home/guo/moSource/perclos_evaluation_svm/build/trainfeaturesOpencv",
  // ios::out);
  // int i=0;
  // for(i=0; i<startnum; i++)
  // {
  //     ofiletrain << trainLabelMat.at<int>(order[i], 0) << " ";
  //     for(int j=0; j<trainFeatureMat.cols; j++)
  //     {
  //         ofiletrain << j+1 << ":" << trainFeatureMat.at<float>(order[i], j);
  //         if(j+1 == trainFeatureMat.cols)
  //         {
  //             ofiletrain << endl;
  //         } else
  //         {
  //             ofiletrain << " ";
  //         }
  //     }
  // }
  // ofiletrain.close();

  // ofstream ofiletest;
  // ofiletest.open("/home/guo/moSource/perclos_evaluation_svm/build/testfeaturesOpencv",
  // ios::out);
  // for(i=startnum ; i<trainFeatureMat.rows; i++)  //行
  // {
  //     ofiletest << trainLabelMat.at<int>(order[i], 0) << " ";
  //     // cout << trainLabelMat.at<float>(i, 0) << " ";
  //     for(int j=0; j<trainFeatureMat.cols; j++)
  //     {
  //         ofiletest << j+1 << ":" << trainFeatureMat.at<float>(order[i], j);
  //         if(j+1 == trainFeatureMat.cols)
  //         {
  //             ofiletest << endl;
  //         } else {
  //             ofiletest << " ";
  //         }
  //     }
  // }

  // ofiletest.close();

  int i = 0;
  ofstream ofileParam;
  ofileParam.open(model_params, ios::out);
  for (i = 0; i < 2; i++)  //行
  {
    for (size_t j = 0; j < mean.size(); j++) {
      if (i == 0) {
        ofileParam << mean[j];
      } else {
        ofileParam << standard[j];
      }

      if (j == mean.size() - 1) {
        ofileParam << "\n";
      } else {
        ofileParam << " ";
      }
    }
  }
  ofileParam.close();

  // FILE *fp = fopen("/home/guo/moSource/perclos_evaluation_svm/build/Param",
  // "w");
  // if(fp==NULL) {
  //    cout << "write error" << endl;
  // }
  // for(i=0 ; i<2; i++)  //行
  // {
  //     for(int j=0; j<mean.size(); j++)
  //     {
  //         if(i == 0)
  //         {
  //             ofileParam << mean[j];
  //         } else {
  //             ofileParam << standard[j];
  //         }

  //         if(j == mean.size()-1)
  //         {
  //             ofileParam << "\n";
  //         } else {
  //             ofileParam << " ";
  //         }
  //     }
  // }
}

void prepare_param() {
  param.svm_type = C_SVC;
  param.kernel_type = RBF;
  param.degree = 3;
  param.gamma = GAMMA;  // 1/num_features
  param.coef0 = 0;
  param.nu = 0.5;
  param.cache_size = 100;
  param.C = SVM_C;
  param.eps = 1e-3;
  param.p = 0.1;
  param.shrinking = 1;
#ifdef TRAINMODEL
  param.probability = 1;
#else
  param.probability = 0;
#endif

  param.nr_weight = 0;
  param.weight_label = NULL;
  param.weight = NULL;
  cross_validation = 0;
}