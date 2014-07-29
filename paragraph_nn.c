#include <stdio.h>
#include "fann.h"

int main(int argc, char *argv[]) {
  if (argc < 4) {
    printf("Usage: ./paragraph_nn <train_data> <test_data> <save_file> <load_file>");
    exit(0);
  }

  const unsigned int num_layers = 3;
  const unsigned int num_neurons_hidden = 50;
  const float desired_error = (const float) 0.001;
  struct fann *ann;
  struct fann_train_data *train_data, *test_data;
  unsigned int i = 0;
  fann_type* output;
  unsigned int errors = 0;
  
  if(argc == 5) {
    printf("Loading network.\n");
    ann = fann_create_from_file(argv[4]);
  } else {
    train_data = fann_read_train_from_file(argv[1]);
  
    ann = fann_create_standard(num_layers, train_data->num_input, num_neurons_hidden, train_data->num_output);
  
    printf("Training network.\n");
  
    fann_set_training_algorithm(ann, FANN_TRAIN_INCREMENTAL);
    fann_set_learning_momentum(ann, 0.4f);
    fann_shuffle_train_data(train_data);
    fann_train_on_data(ann, train_data, 3000, 10, desired_error);
  }
  printf("Testing network.\n");
  
  test_data = fann_read_train_from_file(argv[2]);

  fann_reset_MSE(ann);
  for(i = 0; i < fann_length_train_data(test_data); i++) {
    if(*test_data->output[i] == (fann_type)-1) continue;
    fann_test(ann, test_data->input[i], test_data->output[i]);
    output = fann_run(ann, test_data->input[i]);
    if(*output != *test_data->output[i])
      errors++;
  }
  
  printf("MSE error on test data: %f\n", fann_get_MSE(ann));
  printf("Error rate on test data: %f%%\n", ((float)errors)/fann_length_train_data(test_data) * 100);

  printf("Saving network.\n");
  
  fann_save(ann, argv[3]);

  printf("Cleaning up.\n");
  fann_destroy_train(train_data);
  fann_destroy_train(test_data);
  fann_destroy(ann);
  
  return 0;

}
