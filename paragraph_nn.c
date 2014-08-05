#include <stdio.h>
#include <string.h>
#include "fann.h"

int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
    if (a == argc - 1) {
      printf("Argument missing for %s\n", str);
      exit(1);
    }
    return a;
  }
  return -1;
}

int main(int argc, char *argv[]) {
  int i;
  if (argc == 1) {
    printf("PARAGRAPH VECTOR neural network trainer\n\n");
    printf("Options:\n");
    printf("Parameters for training:\n");
    printf("\t-hidden <int>\n");
    printf("\t\tNumber of neurons in the hidden layer; default 50\n");
    printf("\t-error <float>\n");
    printf("\t\tDesired training error; default 0.001\n");
    printf("\t-epochs <int>\n");
    printf("\t\tMax epochs for training; default 3000\n");
    printf("\t-train <file>\n");
    printf("\t\tName of file with training vectors (See FANN documentation for format.)\n");
    printf("\t-test <file>\n");
    printf("\t\tname of file with test vectors (See FANN documentation for format.)\n");
    printf("\t-network <file>\n");
    printf("\t\tName of file with previously trained network (-train option will be ignored.)\n");
    printf("\t-output <file>\n");
    printf("\t\tName of file to save network.\n");
  }

  unsigned int num_layers = 3;
  unsigned int num_neurons_hidden = 50;
  unsigned int max_epochs = 3000;
  float desired_error = 0.001f;
  char train_file[1000];
  char test_file[1000];
  char network_file[1000];
  char output_file[1000];
  struct fann *ann;
  struct fann_train_data *train_data, *test_data;
  
  fann_type* output;
  unsigned int errors = 0;

  train_file[0] = 0;
  test_file[0] = 0;
  network_file[0] = 0;
  output_file[0] = 0;
  
  if ((i = ArgPos((char *)"-hidden", argc, argv)) > 0) num_neurons_hidden = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-epochs", argc, argv)) > 0) max_epochs = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-error", argc, argv)) > 0) desired_error = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-test", argc, argv)) > 0) strcpy(test_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-network", argc, argv)) > 0) strcpy(network_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  
  if(network_file[0] != 0) {
    printf("Loading network.\n");
    ann = fann_create_from_file(network_file);
  } else if (train_file[0] != 0) {
    printf("Reading training data.\n");
    train_data = fann_read_train_from_file(train_file);
  
    ann = fann_create_standard(num_layers, train_data->num_input, num_neurons_hidden, train_data->num_output);
  
    printf("Training network.\n");
  
    fann_set_training_algorithm(ann, FANN_TRAIN_RPROP);
    fann_set_learning_momentum(ann, 0.4f);
    fann_set_activation_function_output(ann, FANN_LINEAR);
    fann_shuffle_train_data(train_data);
    fann_train_on_data(ann, train_data, max_epochs, 10, desired_error);
  } else
    exit(0);
  
  if(test_file[0] != 0) {
    printf("Testing network.\n");
  
    test_data = fann_read_train_from_file(test_file);

    fann_reset_MSE(ann);
    int num_test_data = fann_length_train_data(test_data);
    for(i = 0; i < num_test_data; i++) {
      if(*test_data->output[i] == (fann_type)-1) continue;
      fann_test(ann, test_data->input[i], test_data->output[i]);
      output = fann_run(ann, test_data->input[i]);
      if(memcmp(output,test_data->output[i],sizeof(output)) != 0)
        errors++;
    }
    
    
    printf("MSE error on test data: %f\n", fann_get_MSE(ann));
    printf("Error rate on test data: %f%%\n", (((float) num_test_data - errors)/num_test_data) * 100);
  }
  
  if(output_file[0] != 0) {
    printf("Saving network.\n");
  
    fann_save(ann, output_file);
  }
  printf("Cleaning up.\n");
  if (train_file[0] != 0)
    fann_destroy_train(train_data);
  if (test_file[0] != 0 )
    fann_destroy_train(test_data);
  fann_destroy(ann);
  
  return 0;

}
