
#include <iostream>
#include <fstream>
#include <algorithm>
#include <stdlib.h>
#include <math.h>
#include "analysis.h"
#include "ast.h"
#include "codegen.h"
#include "simulationUtils.h"
#include "simulator.h"

using Simulator::DataMovementHint;
using Simulator::PageLocation;
using Simulator::MigrationDirection;

extern std::string migration_policy_str;
extern std::string eviction_policy_str;
extern std::vector<Model_Layer*> forward_layers;
extern std::vector<Model_OP*> forward_ops;
extern std::unordered_map<int, Model_OP*> op_map;
extern std::unordered_map<int, OP_tensor> transformer_tensors;
extern double GPU_frequency_GHz;
extern double GPU_memory_size_GB;
extern double CPU_PCIe_bandwidth_GBps;
extern double SSD_PCIe_bandwidth_GBps;
extern double GPU_malloc_uspB;
extern double GPU_free_uspB;
extern int prefetch_degree;
extern int borden;
extern int is_transformer;
double CPU_memory_line_GB = -1;
long CPU_line;
double SSD_latency_us = -1;
double system_latency_us = -1;
double delta_parameter = -1;
double loosen_parameter = 1;
extern int algo_speedup;

long long memory_offset_intermediate = 0;
long long memory_offset_weights = 0;
int kernel_index = 0;
int kernel_num;
int prefetch_optimize = 1;
std::vector<Tensor*> tensor_list;
std::vector<CUDAKernel> kernel_list;
std::vector<double> kernel_time_table;
std::vector<double> kernel_time_table_extended_sort;
std::vector<bool> kernel_no_ongoing_mirgation_list_C2G;
std::vector<bool> kernel_no_ongoing_mirgation_list_G2C;
std::vector<bool> kernel_no_ongoing_mirgation_list_S2G;
std::vector<bool> kernel_no_ongoing_mirgation_list_G2S;
std::vector<Hidding_Interval*> interval_list;
std::vector<EvictionGuide_Entry> EvictionGuide_Table;
std::vector<long> GPU_resident_memory_estimation;
std::vector<long> GPU_resident_memory_estimation_pinned;
std::vector<long> CPU_resident_memory_estimation;
std::vector<DataMovementHint> movement_hints;
std::vector<Offload_Hint_FlashNeuron> offload_hints_fn;
std::vector<Hidding_Interval*> offloeded_local_intervals;

//flashneuron:
std::priority_queue<fl_pending_event, std::vector<fl_pending_event>, Fl_event_less> fl_pending_event_queue;


//FN migration plan output
std::ofstream migration_plan_output;

// extern const std::string print_pagelocation_array[5];

Model_Layer::Model_Layer(){
    operatorr = NULL;
}


Model_Layer::Model_Layer(Operatorr* op, int id){
    N = C = H = W = 0;
    layer_id = id;
    operatorr = op;
}


void Model_Layer::give_next_layer_size(int* N, int* C, int* H, int* W){
    *N = this->N;
    Assert(this->operatorr!=NULL);
    if (this->operatorr->type==OperatorType::Conv2d_T)
    {
        Conv2d* op = dynamic_cast<Conv2d*>(this->operatorr);
        Assert(this->C==op->in_channels);
        *C = op->out_channels;
        *H = ((this->H - op->kernel_size_r + 2 * op->padding_0)/op->stride_0) +1;
        *W = ((this->W - op->kernel_size_s + 2 * op->padding_1)/op->stride_1) +1;
    }
    else if (this->operatorr->type==OperatorType::ReLU_T || this->operatorr->type==OperatorType::BatchNorm2d_T || this->operatorr->type==OperatorType::Dropout_T)
    {
        *C = this->C;
        *H = this->H;
        *W = this->W;
    }
    else if (this->operatorr->type==OperatorType::MaxPool2d_T)
    {
        MaxPool2d* op = dynamic_cast<MaxPool2d*>(this->operatorr);
        *C = this->C;
        *H = ((this->H - op->kernel_size + 2 * op->padding)/op->stride) +1;
        *W = ((this->W - op->kernel_size + 2 * op->padding)/op->stride) +1;
    }
    else if (this->operatorr->type==OperatorType::AdaptiveAvgPool2d_T)
    {
        AdaptiveAvgPool2d* op = dynamic_cast<AdaptiveAvgPool2d*>(this->operatorr);
        *C = this->C;
        *H = op->outputsize_0;
        *W = op->outputsize_1;
    }
    else if (this->operatorr->type==OperatorType::Linear_T)
    {
        Linear* op = dynamic_cast<Linear*>(this->operatorr);
        *C = ((this->C * this->H * this->W ) / op->in_features) * op->out_features; 
        *H = 1;
        *W = 1;
    }
    else if (this->operatorr->type==OperatorType::Add_T)
    {
        *C = this->C;
        *H = this->H;
        *W = this->W;
    } 
    else if (this->operatorr->type==OperatorType::Concat_T)
    {
        *C = this->C;
        *H = this->H;
        *W = this->W;
    }
    else if (this->operatorr->type==OperatorType::Scale_T)
    {
        *C = this->C;
        int numbers[4];
        this->get_scale_num_layer->give_next_layer_size(numbers, numbers+1, numbers+2, numbers+3); 
        *H = numbers[2];
        *W = numbers[3];
        this->scale_H = *H;
        this->scale_W = *W;
    }
    
    
    else{std::cerr<<"Error Operator Type!"<<std::endl;}
}



void layer_pre_pass_datasize(){
    for (int i = 1; i < forward_layers.size(); i++)
    {
        Model_Layer* current_layer = forward_layers[i];
        Assert(current_layer->previous_layers.size()!=0);
        

        if (current_layer->operatorr->type==OperatorType::Concat_T)
        {
            int numbers[4];
            current_layer->previous_layers[0]->give_next_layer_size(numbers, numbers+1, numbers+2, numbers+3); 
            current_layer->N = numbers[0];
            current_layer->H = numbers[2];
            current_layer->W = numbers[3];

            int C_sum  = 0;
            for (int j = 0; j < current_layer->previous_layers.size(); j++)
            {
                int numbers_j[4];
                current_layer->previous_layers[j]->give_next_layer_size(numbers_j, numbers_j+1, numbers_j+2, numbers_j+3); 
                Assert(current_layer->H == numbers_j[2]);
                Assert(current_layer->W == numbers_j[3]);
                current_layer->input_Cs.push_back(numbers_j[1]);
                C_sum += numbers_j[1];
            }
            current_layer->C = C_sum;
        }
        else
        {
            int numbers[4];
            current_layer->previous_layers[0]->give_next_layer_size(numbers, numbers+1, numbers+2, numbers+3); 
            current_layer->N = numbers[0];
            current_layer->C = numbers[1];
            current_layer->H = numbers[2];
            current_layer->W = numbers[3];
        }

        if (current_layer->operatorr->type==Add_T)
        {
            for (int j = 0; j < current_layer->previous_layers.size(); j++)
            {
                int numbers[4];
                current_layer->previous_layers[j]->give_next_layer_size(numbers, numbers+1, numbers+2, numbers+3); 

                Assert(numbers[1]==current_layer->C);
                Assert(numbers[2]==current_layer->H);
                Assert(numbers[3]==current_layer->W);
            }
        }
    }
    
}


string Tensor::name() const {
    return "tensor" + std::to_string(tensor_id);
}

bool Tensor::is_alive(int current_kernel) const {
    return is_global_weight || (live_interval[1] == -1 ? current_kernel == live_interval[0] :
           current_kernel >= live_interval[0] && current_kernel < live_interval[1]);
}


void Tensor::print() const {
    std::cout << "tensor" << tensor_id << 
            " Is weight (global)?: " << this->is_global_weight << ", " <<
            "Size in byte: " << size_in_byte << ", " <<
            "Range:" << address_offset << "--" << 
            address_offset + ((size_in_byte % 4096 == 0) ? 
                    (size_in_byte / 4096) : 
                    ((size_in_byte / 4096) + 1)) * 4096 << 
            std::endl;
}


void Model_Layer::print_name(){
    std::cout<<"Layer ID:"<<layer_id<<"; Name:";
    switch (operatorr->type)
    {
    case OperatorType::AdaptiveAvgPool2d_T :
        std::cout<<"AdaptiveAvgPool2d";
        break;
    
    case OperatorType::BatchNorm2d_T :
        std::cout<<"BatchNorm2d";
        break;
    
    case OperatorType::Conv2d_T :
        std::cout<<"Conv2d";
        break;
    
    case OperatorType::Dropout_T :
        std::cout<<"Dropout";
        break;
    
    case OperatorType::Linear_T :
        std::cout<<"Linear";
        break;
    
    case OperatorType::MaxPool2d_T :
        std::cout<<"MaxPool2d";
        break;
    
    case OperatorType::ReLU_T :
        std::cout<<"ReLU";
        break;

    case OperatorType::Add_T :
        std::cout<<"Add";
        break;

    case OperatorType::Concat_T :
        std::cout<<"Concat";
        break;

    case OperatorType::Scale_T :
        std::cout<<"Scale";
        break;

    default:
        break;
    }
    std::cout<<" ";
    std::cout<<"("<<N<<","<<C<<","<<H<<","<<W<<")"<<std::endl;
}


void Model_Layer::print(){
    print_name();
    std::cout<<"Next Layers:"<<std::endl;
    for (int i = 0; i < next_layers.size(); i++)
    {
        std::cout<<"Next Layer "<<i<<std::endl;
        next_layers[i]->print_name();
    }

    std::cout<<"Previous Layers:"<<std::endl;
    for (int i = 0; i < previous_layers.size(); i++)
    {
        std::cout<<"Previous Layer "<<i<<std::endl;
        previous_layers[i]->print_name();
    }
    
    std::cout<<"Input Tensor: ";
    input_activation->print();
    std::cout<<"Output Tensor: ";
    output_activation->print();
    if (d_input)
    {
        std::cout<<"d_Input Tensor: ";
        d_input->print();
    }
    std::cout<<"d_Output Tensor: ";
    d_output->print();
    if (weight)
    {
        std::cout<<"Weight Tensor: ";
        weight->print();
        std::cout<<"d_Weight Tensor: ";
        d_weight->print();
    }
    if (bias)
    {
        std::cout<<"Bias Tensor: ";
        bias->print();
        std::cout<<"d_Bias Tensor: ";
        d_bias->print();
    }
    if (alpha_and_beta)
    {
        std::cout<<"Alpha_and_Beta Tensor: ";
        alpha_and_beta->print();
        std::cout<<"d_Alpha_and_beta Tensor: ";
        d_alpha_and_beta->print();
    }
    if (mu)
    {
        std::cout<<"Mu Tensor: ";
        mu->print();
        std::cout<<"d_mu Tensor: ";
        d_mu->print();
    }
    if (var)
    {
        std::cout<<"Var Tensor: ";
        var->print();
        std::cout<<"d_var Tensor: ";
        d_var->print();
    }
    if (v1)
    {
        std::cout<<"V1 Tensor: ";
        v1->print();
        std::cout<<"d_v1: ";
        d_v1->print();
    }
    if (v2)
    {
        std::cout<<"V2 Tensor: ";
        v2->print();
        std::cout<<"d_v2: ";
        d_v2->print();
    }
    if (running_m)
    {
        std::cout<<"Running_m Tensor: ";
        running_m->print();
        std::cout<<"Running_v Tensor: ";
        running_v->print();
    }
    if (musk_array)
    {
        std::cout<<"Musk_array Tensor: ";
        musk_array->print();
    }
    for (int i = 0; i < other_inputs.size(); i++)
    {
        std::cout<<"Other Inputs Tensor "<<i<<": ";
        other_inputs[i]->print();
    }
    for (int i = 0; i < other_d_inputs.size(); i++)
    {
        std::cout<<"Other D_Inputs Tensor "<<i<<": ";
        other_d_inputs[i]->print();
    }
    for (int i = 0; i < other_d_outputs.size(); i++)
    {
        std::cout<<"Other D_Outputs Tensor "<<i<<": ";
        other_d_outputs[i]->print();
    }
    std::cout<<"______________________________________________________________________________"<<std::endl;
}

Tensor::Tensor(long long size, bool glob) {
    static int tensor_count = 0;
    tensor_id = tensor_count++;
    size_in_byte = size;
    raw_size_byte = size;
    is_global_weight = glob;
    if (glob)
    {
        address_offset = memory_offset_weights;
        //page-level alignment
        long N_pages = (size % 4096 == 0) ? (size / 4096) : ((size / 4096) + 1);
        memory_offset_weights += N_pages * 4096;
        size_in_byte = N_pages * 4096;
    }
    else
    {
        address_offset = memory_offset_intermediate;
        //page-level alignment
        long N_pages = (size % 4096 == 0) ? (size / 4096) : ((size / 4096) + 1);
        memory_offset_intermediate += N_pages * 4096;
        size_in_byte = N_pages * 4096;
    }
}


Tensor::Tensor(long long size, int tensor_ID, bool glob){
    tensor_id = tensor_ID;
    size_in_byte = size;
    raw_size_byte = size;
    is_global_weight = glob;
    if (glob)
    {
        address_offset = memory_offset_weights;
        //page-level alignment
        long N_pages = (size % 4096 == 0) ? (size / 4096) : ((size / 4096) + 1);
        memory_offset_weights += N_pages * 4096;
        size_in_byte = N_pages * 4096;
    }
    else
    {
        address_offset = memory_offset_intermediate;
        //page-level alignment
        long N_pages = (size % 4096 == 0) ? (size / 4096) : ((size / 4096) + 1);
        memory_offset_intermediate += N_pages * 4096;
        size_in_byte = N_pages * 4096;
    }
}


unsigned long Tensor::getGlobalOffset() {
    return address_offset + (is_global_weight ? 0 : memory_offset_weights);
}



//For transformers
void transformer_op_datalow_pass(int borden){
    for (int i = 0; i < forward_ops.size(); i++)
    {
        Model_OP* curr_op = forward_ops[i];
        //General things:
        //Allocate output:
        if (transformer_tensors.find(curr_op->op_id)!=transformer_tensors.end())
        {
            OP_tensor out = transformer_tensors[curr_op->op_id];
            curr_op->output_dim = out.dim;
            curr_op->output_dims.reserve(out.dim);
            for (int j : out.dims)
            {
                curr_op->output_dims.push_back(j);
            }
            
            if (out.tensor!=nullptr)
            {
                curr_op->output_tensor = out.tensor;
            }
            else
            {
                long size = 1;
                for (int j : out.dims)
                {
                    size = size * j;
                }
                bool global = false;
                if (out.tensor_id < borden)
                {
                    global = true;
                }
                
                curr_op->output_tensor = new Tensor(size*4, global);
                tensor_list.push_back(curr_op->output_tensor);
                curr_op->output_tensor->print();
                transformer_tensors[curr_op->op_id].tensor = curr_op->output_tensor;
            }
            
        }else
        {
            OP_tensor out_size = curr_op->input_tensors[0];
            long size = 1;
            for (auto j : out_size.dims)
            {
                size = size * j;
            }
            bool global = false;
            
            curr_op->output_tensor = new Tensor(size*4, global);
            tensor_list.push_back(curr_op->output_tensor);
            curr_op->output_tensor->print();
        }

        //Allocate Input:
        for (int j = 0; j < curr_op->input_num; j++)
        {
            int id = curr_op->input_tensors[j].tensor_id;

            if (transformer_tensors.find(id)!=transformer_tensors.end())
            {
                OP_tensor in_curr = transformer_tensors[id];
                if (in_curr.tensor!=nullptr)
                {
                    curr_op->input_tensors[j].tensor = in_curr.tensor;
                }
                else
                {
                    long size = 1;
                    for (int j : in_curr.dims)
                    {
                        size = size * j;
                    }
                    bool global = false;
                    if (in_curr.tensor_id < borden)
                    {
                        global = true;
                    }
                    
                    curr_op->input_tensors[j].tensor = new Tensor(size*4, global);
                    tensor_list.push_back(curr_op->input_tensors[j].tensor);
                    curr_op->input_tensors[j].tensor->print();
                    transformer_tensors[id].tensor = curr_op->input_tensors[j].tensor;
                }
            }else
            {
                exit(1);
            }
        }


        for (int j = 0; j < curr_op->input_tensors.size(); j++)
        {
            if (curr_op->type=="Power")
            {
                if (curr_op->input_tensors[j].tensor->is_global_weight)
                {
                    curr_op->input_tensors[j].is_const = true;
                }
                else
                {
                    if (op_map.find(curr_op->input_tensors[j].tensor_id)!=op_map.end())
                    {
                        curr_op->input_tensors[j].d_tensor = new Tensor(curr_op->input_tensors[j].tensor->size_in_byte, false);
                        tensor_list.push_back(curr_op->input_tensors[j].d_tensor);
                        Model_OP* source_op = op_map[curr_op->input_tensors[j].tensor_id];
                        source_op->d_output_tensors.push_back(curr_op->input_tensors[j].d_tensor);
                    }
                    else
                    {
                        curr_op->input_tensors[j].is_const = true;
                    }
                }
            }else
            {
                if (curr_op->input_tensors[j].tensor->is_global_weight)
                {
                    curr_op->input_tensors[j].d_tensor = new Tensor(curr_op->input_tensors[j].tensor->size_in_byte, false);
                    tensor_list.push_back(curr_op->input_tensors[j].d_tensor);
                    if (op_map.find(curr_op->input_tensors[j].tensor_id)!=op_map.end())
                    {
                        Model_OP* source_op = op_map[curr_op->input_tensors[j].tensor_id];
                        source_op->d_output_tensors.push_back(curr_op->input_tensors[j].d_tensor);
                    }
                }else{
                    if (op_map.find(curr_op->input_tensors[j].tensor_id)!=op_map.end())
                    {
                        curr_op->input_tensors[j].d_tensor = new Tensor(curr_op->input_tensors[j].tensor->size_in_byte, false);
                        Model_OP* source_op = op_map[curr_op->input_tensors[j].tensor_id];
                        tensor_list.push_back(curr_op->input_tensors[j].d_tensor);
                        source_op->d_output_tensors.push_back(curr_op->input_tensors[j].d_tensor);
                    }
                    else
                    {
                        curr_op->input_tensors[j].is_const = true;
                    }
                }
            }
        }

        if (i==forward_ops.size()-1)
        {
            Tensor* loss = new Tensor(curr_op->output_tensor->size_in_byte, false);
            tensor_list.push_back(loss);
            curr_op->d_output_tensors.push_back(loss);
        }
        
    }

    for (int i = 0; i < forward_ops.size(); i++)
    {
        forward_ops[i]->print();
    }
    
}



void Model_OP::print(){
    std::cout<<op_id<<": "<<type<<std::endl;
  
    std::cout<<"Input Tensor(s): "<<std::endl;
    for (int i = 0; i < this->input_num; i++)
    {
        this->input_tensors[i].tensor->print();
    }
    
    std::cout<<"Output Tensor: "<<std::endl;
    output_tensor->print();

    std::cout<<"d_Input Tensor(s): "<<std::endl;
    for (int i = 0; i < this->input_num; i++)
    {
        if (this->input_tensors[i].d_tensor)
        {
            this->input_tensors[i].d_tensor->print();
        }
    }

    std::cout<<"d_Output Tensor: "<<std::endl;
    for (int i = 0; i < d_output_tensors.size(); i++)
    {
        d_output_tensors[i]->print();
    }
    
    std::cout<<"______________________________________________________________________________"<<std::endl;
}






void layer_first_pass_dataflow(){

    //Forward propogation
    for (size_t i = 0; i < forward_layers.size(); i++)
    {
        Model_Layer* current_layer = forward_layers[i];
        Assert(current_layer);

        int N, C ,H, W;
        N = current_layer->N;
        C = current_layer->C;
        H = current_layer->H;
        W = current_layer->W;

        if (current_layer->operatorr->type==OperatorType::Conv2d_T)
        {
            Conv2d* op = dynamic_cast<Conv2d*>(current_layer->operatorr);
            int NN, K, P, Q;
            current_layer->give_next_layer_size(&NN, &K, &P, &Q);
            int R = op->kernel_size_r;
            int S = op->kernel_size_s;


            if (i==0)
            {
                current_layer->input_activation = new Tensor((long long) N*C*H*W*4);
                tensor_list.push_back(current_layer->input_activation);
            }
            else
            {
                current_layer->input_activation = current_layer->previous_layers[0]->output_activation; // Previous layer's output
            }

            current_layer->output_activation = new Tensor((long long) N*K*P*Q*4);
            tensor_list.push_back(current_layer->output_activation);
            current_layer->weight = new Tensor((long long) K*C*R*S*4, true);
            tensor_list.push_back(current_layer->weight);
            current_layer->d_weight = new Tensor((long long) K*C*R*S*4);
            tensor_list.push_back(current_layer->d_weight);
            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);

        }
        else if (current_layer->operatorr->type==OperatorType::ReLU_T)
        {
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output 

            ReLU* op = dynamic_cast<ReLU*>(current_layer->operatorr);
            if (!op->inplace)
            {
                current_layer->output_activation = new Tensor((long long) N*C*H*W*4);
                tensor_list.push_back(current_layer->output_activation);
            }
            else
            {
                current_layer->output_activation = current_layer->input_activation;
            }
            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::AdaptiveAvgPool2d_T || current_layer->operatorr->type==OperatorType::MaxPool2d_T)
        {
            int NN, K, P, Q;
            current_layer->give_next_layer_size(&NN, &K, &P, &Q);

            current_layer->input_activation = current_layer->previous_layers[0]->output_activation; //Previous layer's output
            current_layer->output_activation = new Tensor((long long) NN*K*P*Q*4);
            tensor_list.push_back(current_layer->output_activation);
            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::Dropout_T)
        {
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output 

            current_layer->musk_array = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->musk_array);

            Dropout* op = dynamic_cast<Dropout*>(current_layer->operatorr);
            if (!op->inplace)
            {
                current_layer->output_activation = new Tensor((long long) N*C*H*W*4);
                tensor_list.push_back(current_layer->output_activation);
            }
            else
            {
                current_layer->output_activation = current_layer->input_activation;
            }

            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::Linear_T)
        {
            int NN, K, P, Q;
            current_layer->give_next_layer_size(&NN, &K, &P, &Q);
            Linear* op = dynamic_cast<Linear*>(current_layer->operatorr);
            int H_in, H_out;
            H_in = op->in_features;
            H_out = op->out_features;

            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output
            current_layer->weight = new Tensor((long long) H_in*H_out*4, true);
            tensor_list.push_back(current_layer->weight);
            current_layer->output_activation = new Tensor((long long) NN*K*P*Q*4);
            tensor_list.push_back(current_layer->output_activation);

            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);
            current_layer->d_weight = new Tensor((long long) H_in*H_out*4);
            tensor_list.push_back(current_layer->d_weight);

            if (op->bias)
            {
                current_layer->bias = new Tensor((long long) H_out*4, true);
                tensor_list.push_back(current_layer->bias);
                current_layer->d_bias = new Tensor((long long) H_out*4);
                tensor_list.push_back(current_layer->d_bias);
            }
            
        }
        else if (current_layer->operatorr->type==OperatorType::BatchNorm2d_T)
        {
            BatchNorm2d* op = dynamic_cast<BatchNorm2d*>(current_layer->operatorr);
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output

            current_layer->output_activation = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->output_activation);

            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);

            current_layer->alpha_and_beta = new Tensor((long long) C*2*4, true);
            tensor_list.push_back(current_layer->alpha_and_beta);

            current_layer->d_alpha_and_beta = new Tensor((long long) C*2*4);
            tensor_list.push_back(current_layer->d_alpha_and_beta);

            //The following are workspace for BN

            current_layer->mu = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->mu);

            current_layer->var = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->var);

            current_layer->v1 = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->v1);

            current_layer->v2 = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->v2);

            current_layer->d_mu = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->d_mu);

            current_layer->d_var = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->d_var);

            current_layer->d_v1 = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_v1);

            current_layer->d_v2 = new Tensor((long long) C*4);
            tensor_list.push_back(current_layer->d_v2);
            

            if (op->track_running_stats)
            {
                current_layer->running_m = new Tensor((long long) C*4, true);
                tensor_list.push_back(current_layer->running_m);
                current_layer->running_v = new Tensor((long long) C*4, true);
                tensor_list.push_back(current_layer->running_v);
            }
            
        }
        else if (current_layer->operatorr->type==OperatorType::Add_T)
        {
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output
            Assert(current_layer->previous_layers.size()!=1);

            for (int i = 1; i < current_layer->previous_layers.size(); i++)
            {
                current_layer->other_inputs.push_back(current_layer->previous_layers[i]->output_activation);
            }
            
            current_layer->output_activation = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->output_activation);

            //No d_inputs need to be allocated. For Add layer, d_input is d_output
            
        }
        else if (current_layer->operatorr->type==OperatorType::Concat_T)
        {
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;
            Assert(current_layer->previous_layers.size()!=1);

            current_layer->d_input = new Tensor((long long) N*H*W*current_layer->previous_layers[0]->C*4);
            tensor_list.push_back(current_layer->d_input);

            for (int i = 1; i < current_layer->previous_layers.size(); i++)
            {
                current_layer->other_inputs.push_back(current_layer->previous_layers[i]->output_activation);
                Tensor* new_tensor = new Tensor((long long) N*H*W*current_layer->previous_layers[i]->C*4);
                current_layer->other_d_inputs.push_back(new_tensor);
                tensor_list.push_back(new_tensor);
            }

            current_layer->output_activation = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->output_activation);
            
        }
        else if (current_layer->operatorr->type==OperatorType::Scale_T)
        {
            current_layer->input_activation = current_layer->previous_layers[0]->output_activation;  // Previous layer's output 

            int NN, K, P, Q;
            current_layer->give_next_layer_size(&NN, &K, &P, &Q);
            current_layer->output_activation = new Tensor((long long) NN*K*P*Q*4);
            tensor_list.push_back(current_layer->output_activation);

            current_layer->d_input = new Tensor((long long) N*C*H*W*4);
            tensor_list.push_back(current_layer->d_input);
        }
        
        
    }


    for (int i = (int)forward_layers.size() - 1; i >= 0; i--)
    {
        Model_Layer* current_layer = forward_layers[i];

        int N, C ,H, W;
        N = current_layer->N;
        C = current_layer->C;
        H = current_layer->H;
        W = current_layer->W;

        int NN, K, P, Q;
            current_layer->give_next_layer_size(&NN, &K, &P, &Q);

        if (i==forward_layers.size() - 1)
        {
            current_layer->d_output = new Tensor((long long) NN*K*P*Q*4);
            tensor_list.push_back(current_layer->d_output);
        }
        else
        {
            for (int i = 0; i < current_layer->next_layers.size(); i++)
            {
                Model_Layer* one_of_next_layer = current_layer->next_layers[i];
                if (one_of_next_layer->operatorr->type==OperatorType::Add_T)
                {
                    if (i==0)
                    {
                        current_layer->d_output = one_of_next_layer->d_output;
                    }
                    else
                    {
                        current_layer->other_d_outputs.push_back(one_of_next_layer->d_output);
                    }
                }
                else if (one_of_next_layer->operatorr->type==OperatorType::Concat_T)
                {
                    /* code */
                    //First find which index current_layer is in this concat layer's input list;
                    int index = -1;
                    for (int j = 0; j < one_of_next_layer->previous_layers.size(); j++)
                    {
                        if (one_of_next_layer->previous_layers[j]==current_layer)
                        {
                            index = j;
                            break;
                        }
                    }
                    Assert(index>=0);

                    if (i==0)
                    {
                        if (index==0)
                        {
                            current_layer->d_output = one_of_next_layer->d_input;
                        }
                        else
                        {
                            current_layer->d_output = one_of_next_layer->other_d_inputs[index-1];
                        }
                    }
                    else
                    {
                        if (index==0)
                        {
                            current_layer->other_d_outputs.push_back(one_of_next_layer->d_input);
                        }
                        else
                        {
                            current_layer->other_d_outputs.push_back(one_of_next_layer->other_d_inputs[index-1]);
                        }
                    }
                }
                else
                {
                    if (i==0)
                    {
                        current_layer->d_output = one_of_next_layer->d_input;
                    }
                    else
                    {
                        current_layer->other_d_outputs.push_back(one_of_next_layer->d_input);
                    }
                }
                
                
            }
        }
    }
}


CUDAKernel::CUDAKernel(CUDAKernelType t, Model_Layer* layer){
    kernel_id = kernel_index;
    kernel_index++;
    type = t;
    parent_layer = layer;
}


CUDAKernel::CUDAKernel(CUDAKernelType t, Model_OP* op){
    kernel_id = kernel_index;
    kernel_index++;
    type = t;
    parent_op = op;
}


CUDAKernel::CUDAKernel(CUDAKernelType t, int id, long exe_time_ns){
    kernel_id = id;
    kernel_index++;
    type = t;
    execution_cycles = (long) (GPU_frequency_GHz * exe_time_ns);
}


void CUDAKernel::print(){
    std::cout<<"Kernel ID: "<<kernel_id<<", "<< "Name: "<<print_kerneltype_array[type]<<std::endl;
    if (this->parent_layer)
    {
        std::cout<<"Parent Layer ID:"<<parent_layer->layer_id<<"; Name:";
        switch (parent_layer->operatorr->type)
        {
            case OperatorType::AdaptiveAvgPool2d_T :
                std::cout<<"AdaptiveAvgPool2d";
                break;
            
            case OperatorType::BatchNorm2d_T :
                std::cout<<"BatchNorm2d";
                break;
            
            case OperatorType::Conv2d_T :
                std::cout<<"Conv2d";
                break;
            
            case OperatorType::Dropout_T :
                std::cout<<"Dropout";
                break;
            
            case OperatorType::Linear_T :
                std::cout<<"Linear";
                break;
            
            case OperatorType::MaxPool2d_T :
                std::cout<<"MaxPool2d";
                break;
            
            case OperatorType::ReLU_T :
                std::cout<<"ReLU";
                break;

            case OperatorType::Add_T :
                std::cout<<"Add";
                break;

            case OperatorType::Concat_T :
                std::cout<<"Concat";
                break;

            case OperatorType::Scale_T :
                std::cout<<"Scale";
                break;

            default:
                break;
        }
        std::cout<<std::endl;
    }
    else
    {
        //  std::cout<<"Parent OP ID:"<<parent_op->op_id<<"; Name: "<<parent_op->type<<std::endl;
    }
    
    
    std::cout<<"Execution Time: "<< execution_cycles<<std::endl;
    if (this->parent_layer)
    {
        std::cout<<"("<<parent_layer->N<<","<<parent_layer->C<<","<<parent_layer->H<<","<<parent_layer->W<<")"<<std::endl;
    }

    std::cout<<"Input Tensors:"<<std::endl;
    for (auto it = inputs.begin(); it != inputs.end(); it++)
    {
        (*it)->print();
    }
    std::cout<<"Output Tensors:"<<std::endl;
    for (auto it = outputs.begin(); it != outputs.end(); it++)
    {
        (*it)->print();
    }
    std::cout<<"____________________________________________________________________"<<std::endl;
}

void CUDAKernel::getRequiredTensors(std::vector<Tensor*> &required_tensors) const {
  std::unordered_set<Tensor *> set;
  getRequiredTensors(set);
  for (Tensor *tensor : set)
    required_tensors.push_back(tensor);
}

void CUDAKernel::getRequiredTensors(std::unordered_set<Tensor *> &required_tensors) const {
  for (Tensor *tensor : inputs)
    required_tensors.insert(tensor);
  for (Tensor *tensor : outputs)
    required_tensors.insert(tensor);
}

void CUDAKernel::getRequiredTensors(std::vector<Tensor*> &required_tensors,
                                    std::vector<Tensor*> &required_input_tensors,
                                    std::vector<Tensor*> &required_output_tensors) const {
  std::unordered_set<Tensor *> set;
  for (Tensor *tensor : inputs) {
    set.insert(tensor);
    required_tensors.push_back(tensor);
    required_input_tensors.push_back(tensor);
  }
  for (Tensor *tensor : outputs) {
    if (set.find(tensor) == set.end()) {
        required_tensors.push_back(tensor);
        required_output_tensors.push_back(tensor);
    }
  }
}

void CUDAKernel::getTensorBreakdown(std::vector<Tensor*> &inputs,
                                    std::vector<Tensor*> &weights,
                                    std::vector<Tensor*> &intermediates) const {
  std::unordered_set<Tensor *> required_tensors;
  getRequiredTensors(required_tensors);
  Model_Layer* current_layer = parent_layer;
  if (kernel_id == 0) {
    for (Tensor *tensor : this->outputs) {
      inputs.push_back(tensor);
      required_tensors.erase(tensor);
    }
  }
  for (Tensor *tensor : required_tensors) {
    if (tensor->is_global_weight)
      weights.push_back(tensor);
    else
      intermediates.push_back(tensor);
  }
}

void layer_second_pass_scheduling_kernels(){
    //Forward propogation
    for (size_t i = 0; i < forward_layers.size(); i++)
    {
        Model_Layer* current_layer = forward_layers[i];
    
        if (current_layer->operatorr->type==OperatorType::Conv2d_T)
        {
            if (i == 0) //A0
            {
                kernel_list.emplace_back(CUDAKernelType::LoadData_A0, current_layer);
                kernel_list.back().outputs.insert(current_layer->input_activation);
            }

            kernel_list.emplace_back(CUDAKernelType::Conv2d_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->weight);
            kernel_list.back().outputs.insert(current_layer->output_activation);

        }
        else if (current_layer->operatorr->type==OperatorType::ReLU_T)
        {
            kernel_list.emplace_back(CUDAKernelType::ReLU_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        else if (current_layer->operatorr->type==OperatorType::AdaptiveAvgPool2d_T)
        {
            kernel_list.emplace_back(CUDAKernelType::AdaptiveAvgPool2d_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        else if (current_layer->operatorr->type==OperatorType::MaxPool2d_T)
        {
            kernel_list.emplace_back(CUDAKernelType::MaxPool2d_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        
        else if (current_layer->operatorr->type==OperatorType::Dropout_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Dropout_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->musk_array);
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        else if (current_layer->operatorr->type==OperatorType::Linear_T)
        {
            Linear* op = dynamic_cast<Linear*>(current_layer->operatorr);
            kernel_list.emplace_back(CUDAKernelType::Linear_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->weight);
            if (op->bias)
            {
                kernel_list.back().inputs.insert(current_layer->bias);
            }
            kernel_list.back().outputs.insert(current_layer->output_activation);
            
        }
        else if (current_layer->operatorr->type==OperatorType::BatchNorm2d_T)
        {
            BatchNorm2d* op = dynamic_cast<BatchNorm2d*>(current_layer->operatorr);
            kernel_list.emplace_back(CUDAKernelType::BatchNorm2d_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->alpha_and_beta);
            if (op->track_running_stats)
            {
                kernel_list.back().inputs.insert(current_layer->running_m);
                kernel_list.back().inputs.insert(current_layer->running_v);
                kernel_list.back().outputs.insert(current_layer->running_m);
                kernel_list.back().outputs.insert(current_layer->running_v);
            }
            kernel_list.back().outputs.insert(current_layer->output_activation);
            kernel_list.back().outputs.insert(current_layer->mu);
            kernel_list.back().outputs.insert(current_layer->var);
            kernel_list.back().outputs.insert(current_layer->v1);
            kernel_list.back().outputs.insert(current_layer->v2);
        }
        else if (current_layer->operatorr->type==OperatorType::Add_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Add_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            for (int i = 0; i < current_layer->other_inputs.size(); i++)
            {
                kernel_list.back().inputs.insert(current_layer->other_inputs[i]);
            }
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        else if (current_layer->operatorr->type==OperatorType::Concat_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Concat_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            for (int i = 0; i < current_layer->other_inputs.size(); i++)
            {
                kernel_list.back().inputs.insert(current_layer->other_inputs[i]);
            }
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }
        else if (current_layer->operatorr->type==OperatorType::Scale_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Scale_Forward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->output_activation);
        }

    }

    //Make loss 
    kernel_list.emplace_back(CUDAKernelType::makeLoss, forward_layers[forward_layers.size()-1]);
    kernel_list.back().inputs.insert(forward_layers[forward_layers.size()-1]->output_activation);
    kernel_list.back().outputs.insert(forward_layers[forward_layers.size()-1]->d_output);

    //Backward Propogation
    for (int i = (int)forward_layers.size() - 1; i >= 0; i--)
    {
        Model_Layer* current_layer = forward_layers[i];

        if (current_layer->other_d_outputs.size()!=0)
        {
            kernel_list.emplace_back(CUDAKernelType::Add_MultiGredient, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            for (int i = 0; i < current_layer->other_d_outputs.size(); i++)
            {
                kernel_list.back().inputs.insert(current_layer->other_d_outputs[i]);
            }
            kernel_list.back().outputs.insert(current_layer->d_output);
        }
        

        if (current_layer->operatorr->type==OperatorType::Conv2d_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Conv2d_Backward_Weight, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->d_weight);

            kernel_list.emplace_back(CUDAKernelType::Conv2d_Backward_Input, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().inputs.insert(current_layer->weight);
            kernel_list.back().outputs.insert(current_layer->d_input);

            kernel_list.emplace_back(CUDAKernelType::Conv2d_Apply_Grad, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_weight);
            kernel_list.back().inputs.insert(current_layer->weight);
            kernel_list.back().outputs.insert(current_layer->weight);

        }
        else if (current_layer->operatorr->type==OperatorType::ReLU_T)
        {
            kernel_list.emplace_back(CUDAKernelType::ReLU_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::AdaptiveAvgPool2d_T)
        {
            kernel_list.emplace_back(CUDAKernelType::AdaptiveAvgPool2d_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::MaxPool2d_T)
        {
            kernel_list.emplace_back(CUDAKernelType::MaxPool2d_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);
        }
        else if (current_layer->operatorr->type==OperatorType::Dropout_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Dropout_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->musk_array);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);

        }
        else if (current_layer->operatorr->type==OperatorType::Linear_T)
        {
            Linear* op = dynamic_cast<Linear*>(current_layer->operatorr);
            if (op->bias)
            {
                kernel_list.emplace_back(CUDAKernelType::Linear_Backward_Bias, current_layer);
                kernel_list.back().inputs.insert(current_layer->d_output);
                kernel_list.back().outputs.insert(current_layer->d_bias);

                kernel_list.emplace_back(CUDAKernelType::Linear_Apply_Grad_Bias, current_layer);
                kernel_list.back().inputs.insert(current_layer->d_bias);
                kernel_list.back().inputs.insert(current_layer->bias);
                kernel_list.back().outputs.insert(current_layer->bias);
            }
            
            kernel_list.emplace_back(CUDAKernelType::Linear_Backward_Weight, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().inputs.insert(current_layer->input_activation);
            kernel_list.back().outputs.insert(current_layer->d_weight);

            kernel_list.emplace_back(CUDAKernelType::Linear_Backward_Input, current_layer);
            kernel_list.back().inputs.insert(current_layer->weight);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);

            kernel_list.emplace_back(CUDAKernelType::Linear_Apply_Grad_Weight, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_weight);
            kernel_list.back().inputs.insert(current_layer->weight);
            kernel_list.back().outputs.insert(current_layer->weight);
            
        }
        else if (current_layer->operatorr->type==OperatorType::BatchNorm2d_T)
        {
            BatchNorm2d* op = dynamic_cast<BatchNorm2d*>(current_layer->operatorr);

            kernel_list.emplace_back(CUDAKernelType::BatchNorm2d_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().inputs.insert(current_layer->v1);
            kernel_list.back().inputs.insert(current_layer->v2);
            kernel_list.back().inputs.insert(current_layer->alpha_and_beta);
            kernel_list.back().outputs.insert(current_layer->d_input);
            kernel_list.back().outputs.insert(current_layer->d_alpha_and_beta);
            kernel_list.back().outputs.insert(current_layer->d_mu);
            kernel_list.back().outputs.insert(current_layer->d_var);
            kernel_list.back().outputs.insert(current_layer->d_v1);
            kernel_list.back().outputs.insert(current_layer->d_v2);

            kernel_list.emplace_back(CUDAKernelType::BatchNorm2d_Apply_Grad, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_alpha_and_beta);
            kernel_list.back().inputs.insert(current_layer->alpha_and_beta);
            kernel_list.back().outputs.insert(current_layer->alpha_and_beta);
        }
        else if (current_layer->operatorr->type==OperatorType::Concat_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Concat_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);
            for (int j = 0; j < current_layer->other_d_inputs.size(); j++)
            {
                kernel_list.back().outputs.insert(current_layer->other_d_inputs[j]);
            }
        }
        else if (current_layer->operatorr->type==OperatorType::Scale_T)
        {
            kernel_list.emplace_back(CUDAKernelType::Scale_Backward, current_layer);
            kernel_list.back().inputs.insert(current_layer->d_output);
            kernel_list.back().outputs.insert(current_layer->d_input);
        }
        
        
    }
}



void transformer_scheduling_kernels(){
    //Forward propogation
    for (size_t i = 0; i < forward_ops.size(); i++)
    {
        Model_OP* current_op = forward_ops[i];
        
        if (current_op->type=="GatherV2")
        {
            kernel_list.emplace_back(CUDAKernelType::GatherV2_Forward, current_op);
        }
        else if (current_op->type=="Convolution")
        {
            kernel_list.emplace_back(CUDAKernelType::Conv2d_Forward, current_op);
        }
        else if (current_op->type=="Dot")
        {
            kernel_list.emplace_back(CUDAKernelType::Linear_Forward, current_op);
        }
        else if (current_op->type=="Relu")
        {
            kernel_list.emplace_back(CUDAKernelType::ReLU_Forward, current_op);
        }
        else if (current_op->type=="Add")
        {
            kernel_list.emplace_back(CUDAKernelType::Add_Forward, current_op);
        }
        else if (current_op->type=="BatchMatMul")
        {
            kernel_list.emplace_back(CUDAKernelType::BatchMatMul_Forward, current_op);
        }
        else if (current_op->type=="Divide")
        {
            kernel_list.emplace_back(CUDAKernelType::Divide_Forward, current_op);
        }
        else if (current_op->type=="Multiply")
        {
            kernel_list.emplace_back(CUDAKernelType::Multiply_Forward, current_op);
        }
        else if (current_op->type=="Power")
        {
            kernel_list.emplace_back(CUDAKernelType::Power_Forward, current_op);
        }
        else if (current_op->type=="SoftmaxBasic")
        {
            kernel_list.emplace_back(CUDAKernelType::SoftmaxBasic_Forward, current_op);
        }
        else if (current_op->type=="Sqrt")
        {
            kernel_list.emplace_back(CUDAKernelType::Sqrt_Forward, current_op);
        }
        else if (current_op->type=="Subtract")
        {
            kernel_list.emplace_back(CUDAKernelType::Subtract_Forward, current_op);
        }
        else if (current_op->type=="Sum")
        {
            kernel_list.emplace_back(CUDAKernelType::Sum_Forward, current_op);
        }
        else if (current_op->type=="Tanh")
        {
            kernel_list.emplace_back(CUDAKernelType::Tanh_Forward, current_op);
        }
        else if (current_op->type=="Erf")
        {
            kernel_list.emplace_back(CUDAKernelType::Erf_Forward, current_op);
        }
        else{
            exit(1);
        }

        for (int j = 0; j < current_op->input_num; j++)
        {
            kernel_list.back().inputs.insert(current_op->input_tensors[j].tensor);
        }
        kernel_list.back().outputs.insert(current_op->output_tensor);
    }

    //makeLoss
    kernel_list.emplace_back(CUDAKernelType::makeLoss, forward_ops[forward_ops.size()-1]);
    kernel_list.back().inputs.insert(forward_ops[forward_ops.size()-1]->output_tensor);
    kernel_list.back().outputs.insert(forward_ops[forward_ops.size()-1]->d_output_tensors[0]);


    //Backward_pass
    for (int i = (int)forward_ops.size() - 1; i >= 0; i--){

        Model_OP* current_op = forward_ops[i];

        //First fuse the multiple d_outputs:
        if (current_op->d_output_tensors.size()>1)
        {
            kernel_list.emplace_back(CUDAKernelType::Add_MultiGredient, current_op);
            for (int j = 0; j < current_op->d_output_tensors.size(); j++)
            {
                kernel_list.back().inputs.insert(current_op->d_output_tensors[j]);
            }
            kernel_list.back().outputs.insert(current_op->d_output_tensors[0]);
        }

        //Go
        
        if (current_op->type=="GatherV2")
        {
            kernel_list.emplace_back(CUDAKernelType::GatherV2_Backward, current_op);
            Tensor* d_input = nullptr;
            Tensor* d_weight = nullptr;
            Tensor* input = nullptr;
            Tensor* weight = nullptr;

            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    d_weight = current_op->input_tensors[j].d_tensor;
                    weight = current_op->input_tensors[j].tensor;
                }
                else
                {
                    input = current_op->input_tensors[j].tensor;
                    if (current_op->input_tensors[j].d_tensor)
                    {
                        d_input = current_op->input_tensors[j].d_tensor;
                    }
                }
            }

            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(input);
            kernel_list.back().outputs.insert(d_weight);
            if (d_input)
            {
                kernel_list.back().outputs.insert(d_input);
            }

            kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
            kernel_list.back().inputs.insert(d_weight);
            kernel_list.back().inputs.insert(weight);
            kernel_list.back().outputs.insert(weight);

    
        }
        else if (current_op->type=="Dot")
        {
            Tensor* d_input = nullptr;
            Tensor* d_weight = nullptr;
            Tensor* input = nullptr;
            Tensor* weight = nullptr;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    d_weight = current_op->input_tensors[j].d_tensor;
                    weight = current_op->input_tensors[j].tensor;
                }
                else
                {
                    input = current_op->input_tensors[j].tensor;
                    d_input = current_op->input_tensors[j].d_tensor;
                }
            }


            kernel_list.emplace_back(CUDAKernelType::Linear_Backward_Weight, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(input);
            kernel_list.back().outputs.insert(d_weight);

            if (d_input)
            {
                kernel_list.emplace_back(CUDAKernelType::Linear_Backward_Input, current_op);
                kernel_list.back().inputs.insert(weight);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().outputs.insert(d_input);

            }
            
            kernel_list.emplace_back(CUDAKernelType::Linear_Apply_Grad_Weight, current_op);
            kernel_list.back().inputs.insert(d_weight);
            kernel_list.back().inputs.insert(weight);
            kernel_list.back().outputs.insert(weight);

        }
        else if (current_op->type=="Convolution")
        {
            Tensor* d_input = nullptr;
            Tensor* d_weight = nullptr;
            Tensor* input = nullptr;
            Tensor* weight = nullptr;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    d_weight = current_op->input_tensors[j].d_tensor;
                    weight = current_op->input_tensors[j].tensor;
                }
                else
                {
                    input = current_op->input_tensors[j].tensor;
                    if (current_op->input_tensors[j].d_tensor)
                    {
                        d_input = current_op->input_tensors[j].d_tensor;
                    }
                }
            }

            kernel_list.emplace_back(CUDAKernelType::Conv2d_Backward_Weight, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(input);
            kernel_list.back().outputs.insert(d_weight);

            if(d_input){
                kernel_list.emplace_back(CUDAKernelType::Conv2d_Backward_Input, current_op);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().inputs.insert(weight);
                kernel_list.back().outputs.insert(d_input);
            }

            kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
            kernel_list.back().inputs.insert(d_weight);
            kernel_list.back().inputs.insert(weight);
            kernel_list.back().outputs.insert(weight);
        }
        
        else if (current_op->type=="Relu")
        {
            kernel_list.emplace_back(CUDAKernelType::ReLU_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="Add")
        {
            kernel_list.emplace_back(CUDAKernelType::Add_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            int global_id = -1;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].d_tensor)
                {
                    kernel_list.back().outputs.insert(current_op->input_tensors[j].d_tensor);
                }
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    global_id = j;
                }
            }
            if (global_id!=-1)
            {
                kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].tensor);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].d_tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[global_id].tensor);
            }
        
        }
        else if (current_op->type=="BatchMatMul")
        {
            if (current_op->input_tensors[0].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::BatchMatMul_Backward, current_op);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().inputs.insert(current_op->input_tensors[1].tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
            }
            if (current_op->input_tensors[1].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::BatchMatMul_Backward, current_op);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);                
                kernel_list.back().outputs.insert(current_op->input_tensors[1].d_tensor);
            }

            int global_id = -1;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    global_id = j;
                }
            }
            if (global_id!=-1)
            {
                kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].tensor);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].d_tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[global_id].tensor);
            }
        }
        else if (current_op->type=="Divide")
        {
            if (current_op->input_tensors[0].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::Divide_Backward_A, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[1].tensor);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
            }
            if (current_op->input_tensors[1].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::Divide_Backward_B, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[1].tensor);
                kernel_list.back().inputs.insert(current_op->output_tensor);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().outputs.insert(current_op->input_tensors[1].d_tensor);
            }

            int global_id = -1;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    global_id = j;
                }
            }
            if (global_id!=-1)
            {
                kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].tensor);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].d_tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[global_id].tensor);
            }
        }
        else if (current_op->type=="Multiply")
        {
            if (current_op->input_tensors[0].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::Multiply_Backward, current_op);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().inputs.insert(current_op->input_tensors[1].tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
            }
            if (current_op->input_tensors[1].d_tensor)
            {
                kernel_list.emplace_back(CUDAKernelType::Multiply_Backward, current_op);
                kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
                kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);                
                kernel_list.back().outputs.insert(current_op->input_tensors[1].d_tensor);
            }

            int global_id = -1;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    global_id = j;
                }
            }
            if (global_id!=-1)
            {
                kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].tensor);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].d_tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[global_id].tensor);
            }
        }
        else if (current_op->type=="Power")
        {
            kernel_list.emplace_back(CUDAKernelType::Power_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().inputs.insert(current_op->input_tensors[1].tensor);
            Assert(current_op->input_tensors[0].d_tensor);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="SoftmaxBasic")
        {
            kernel_list.emplace_back(CUDAKernelType::SoftmaxBasic_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="Sqrt")
        {
            kernel_list.emplace_back(CUDAKernelType::Sqrt_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="Subtract")
        {
            kernel_list.emplace_back(CUDAKernelType::Subtract_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            int global_id = -1;
            for (int j = 0; j < current_op->input_num; j++)
            {
                if (current_op->input_tensors[j].d_tensor)
                {
                    kernel_list.back().outputs.insert(current_op->input_tensors[j].d_tensor);
                }
                if (current_op->input_tensors[j].tensor->is_global_weight)
                {
                    global_id = j;
                }
            }
            if (global_id!=-1)
            {
                kernel_list.emplace_back(CUDAKernelType::Apply_Grad, current_op);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].tensor);
                kernel_list.back().inputs.insert(current_op->input_tensors[global_id].d_tensor);
                kernel_list.back().outputs.insert(current_op->input_tensors[global_id].tensor);
            }
        }
        else if (current_op->type=="Sum")
        {
            kernel_list.emplace_back(CUDAKernelType::Sum_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="Tanh")
        {
            kernel_list.emplace_back(CUDAKernelType::Tanh_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else if (current_op->type=="Erf")
        {
            kernel_list.emplace_back(CUDAKernelType::Erf_Backward, current_op);
            kernel_list.back().inputs.insert(current_op->d_output_tensors[0]);
            kernel_list.back().inputs.insert(current_op->input_tensors[0].tensor);
            kernel_list.back().outputs.insert(current_op->input_tensors[0].d_tensor);
        }
        else{
            exit(1);
        }
        
    }
}



void tensor_first_pass_liveness_analysis(){
    const int tensor_num = tensor_list.size();
    const int kernel_num = kernel_list.size();


    for (int i = 0; i < tensor_num; i++)
    {
        Tensor* current_tensor =  tensor_list[i];
        current_tensor->live_interval[0] = -1;
        current_tensor->live_interval[1] = -1;
        if (!current_tensor->is_global_weight) // This tensor is a local one
        {
            // First we need to find its death time:
            bool find = false;
            for (int j = kernel_num - 1; j >= 0; j--)
            {
                if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end())
                {
                    find = true;
                    current_tensor->live_interval[1] = j+1;
                    break;
                }
            }
            if (!find)
            {
                current_tensor->live_interval[1] = -1;
            }
            
            //Second we need to find its birth time
            for (int j = 0; j < kernel_num; j++)
            {
                if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end() || kernel_list[j].outputs.find(current_tensor)!=kernel_list[j].outputs.end())
                {
                    current_tensor->live_interval[0] = j;
                    break;
                }
            }   
        }
    }
}

void Tensor::print_liveness(){
    this->print();
    if (!this->is_global_weight)
    {
        std::cout<<"Liveness: Birth: "<<this->live_interval[0]<<", Death: "<<this->live_interval[1]<<"."<<std::endl;
    }
    else{
        std::cout<<"Global!"<<std::endl;
    }
    std::cout<<"____________________________________________________________"<<std::endl;
}



void tensor_second_pass_interval_formation(){
    const int tensor_num = tensor_list.size();
    const int kernel_num = kernel_list.size();
    long target_mem_line = (long)(GPU_memory_size_GB * 1024 * 1024 * 1024);

    for (int i = 0; i < tensor_num; i++)
    {
        Tensor* current_tensor = tensor_list[i];
        if (current_tensor->live_interval[0]==-1 && !current_tensor->is_global_weight)
        {
            continue;
        }
        
        if (!current_tensor->is_global_weight)
        {
            if (current_tensor->live_interval[1]!=-1)
            {
                bool a_interval_started = false;
                for (int j = current_tensor->live_interval[0]; j < current_tensor->live_interval[1]; j++)
                {
                    //j is the current kernel;
                    if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end() || kernel_list[j].outputs.find(current_tensor)!=kernel_list[j].outputs.end())
                    {
                        if (!a_interval_started)
                        {
                            if (j+1 < current_tensor->live_interval[1] && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                        }
                        else
                        {
                            interval_list.back()->kernelLevel_interval[1] = j;
                            a_interval_started = false;

                            if (j+1 < current_tensor->live_interval[1] && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                        }
                    }
                    
                }
                Assert(!a_interval_started);
            }
        }
        else
        {
            //This tensor is global
            //First find one use 
            int one_use = 0;
            for (int j = 0; j < kernel_num; j++)
            {
                if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end() || kernel_list[j].outputs.find(current_tensor)!=kernel_list[j].outputs.end())
                {
                    one_use = j;
                    break;
                }
            }




            bool a_interval_started = false;
                for (int j = one_use; j < kernel_num; j++)
                {
                    //j is the current kernel;
                    if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end() || kernel_list[j].outputs.find(current_tensor)!=kernel_list[j].outputs.end())
                    {
                        if (!a_interval_started)
                        {
                            if (j+1 < kernel_num && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                            else if (j+1 == kernel_num && kernel_list[0].inputs.find(current_tensor)==kernel_list[0].inputs.end() && kernel_list[0].outputs.find(current_tensor)==kernel_list[0].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = 0;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                        }
                        else
                        {
                            interval_list.back()->kernelLevel_interval[1] = j;
                            a_interval_started = false;

                            if (j+1 < kernel_num && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                            else if (j+1 == kernel_num && kernel_list[0].inputs.find(current_tensor)==kernel_list[0].inputs.end() && kernel_list[0].outputs.find(current_tensor)==kernel_list[0].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = 0;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                        }
                    }
                }
                for (int j = 0; j <= one_use; j++)
                {
                    //j is the current kernel;
                    if (kernel_list[j].inputs.find(current_tensor)!=kernel_list[j].inputs.end() || kernel_list[j].outputs.find(current_tensor)!=kernel_list[j].outputs.end())
                    {
                        if (!a_interval_started)
                        {
                            if (j<one_use && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                            
                        }
                        else
                        {
                            interval_list.back()->kernelLevel_interval[1] = j;
                            if (j < interval_list.back()->kernelLevel_interval[0])
                            {
                                interval_list.back()->is_looped = 1;
                            }
                            a_interval_started = false;

                            if (j<one_use && kernel_list[j+1].inputs.find(current_tensor)==kernel_list[j+1].inputs.end() && kernel_list[j+1].outputs.find(current_tensor)==kernel_list[j+1].outputs.end())
                            {
                                //Start one interval
                                Hidding_Interval* new_interval = new Hidding_Interval(current_tensor, target_mem_line);
                                new_interval->kernelLevel_interval[0] = j+1;
                                a_interval_started = true;
                                current_tensor->hidding_intervals.push_back(new_interval);
                                interval_list.push_back(new_interval);
                            }
                        }
                    }
                }
                
            Assert(!a_interval_started);
        }
    }
}



void Tensor::print_intervals(){
    print();
    std::cout<<"Hidding Intervals:"<<std::endl;
    for (int i = 0; i < hidding_intervals.size(); i++)
    {
        std::cout<<"interval "<<i<<": "<<hidding_intervals[i]->kernelLevel_interval[0]<<"--------"<<hidding_intervals[i]->kernelLevel_interval[1]<<std::endl;
        std::cout<<"Estimated Time:"<<hidding_intervals[i]->time_estimated<<std::endl;
    }
    std::cout<<"_______________________________________________________________"<<std::endl;
    
}



void get_interval_time(){
    int kernel_num = kernel_list.size();

    //Setup a cumulative time list;
    double time = 0;
    kernel_time_table.push_back(0);
    for (int i = 0; i < kernel_num; i++)
    {
        time += (double)kernel_list[i].execution_cycles / (double)(GPU_frequency_GHz*1000);
        kernel_time_table.push_back(time);
    }

    //Fill the looped extend kernel time table      0 - 2 * kernel_num
    std::vector<double> kernel_time_table_extended;
    kernel_time_table_extended.resize(kernel_num);
    for (int j = 0; j < kernel_num; j++)
    {
        kernel_time_table_extended[j] = kernel_time_table[j];
    }
    double last_time = kernel_time_table[kernel_num];
    kernel_time_table_extended.push_back(last_time);
    for (int j = 0; j < kernel_num; j++)
    {
        last_time += (double)kernel_list[j].execution_cycles / (double)(GPU_frequency_GHz*1000);
        kernel_time_table_extended.push_back(last_time);
    }


    for (int i = 0; i < interval_list.size(); i++)
    {
        
        if (!interval_list[i]->is_looped)
        {
            Assert(interval_list[i]->kernelLevel_interval[1] > interval_list[i]->kernelLevel_interval[0]);
            interval_list[i]->time_estimated = kernel_time_table[interval_list[i]->kernelLevel_interval[1]] - kernel_time_table[interval_list[i]->kernelLevel_interval[0]];
        }
        else
        {
            Assert(interval_list[i]->kernelLevel_interval[1] < interval_list[i]->kernelLevel_interval[0]);
            int end = interval_list[i]->kernelLevel_interval[1];
            int start = interval_list[i]->kernelLevel_interval[0];
            end += kernel_num;
            interval_list[i]->time_estimated = kernel_time_table_extended[end] - kernel_time_table_extended[start];
        }
    }

    //Sort the intervals
    // std::sort(interval_list.begin(), interval_list.end(), [](Hidding_Interval* a, Hidding_Interval* b){
    //     return (a->time_estimated * a->the_tensor->size_in_byte) > (b->time_estimated * b->the_tensor->size_in_byte);
    // });

    // for (int i = 0; i < interval_list.size(); i++)
    // {
    //     interval_list[i]->print();
    // }
}


void give_eviction_guide(){
    int kernel_num = kernel_list.size();
    EvictionGuide_Table.reserve(kernel_num);
    EvictionGuide_Table.resize(kernel_num);


    double medium_interval_time = interval_list[(interval_list.size()-1)/2]->time_estimated;
    

    for (int i = 0; i < tensor_list.size(); i++)
    {
        Tensor* curr_tensor = tensor_list[i];

        double cold_medium_line = medium_interval_time / 3;
        double medium_hot_line = cold_medium_line * 2;

        double ssd_safe_time = 2*(SSD_latency_us + system_latency_us + curr_tensor->size_in_byte / (double)(SSD_PCIe_bandwidth_GBps*1024*1024*1024/1000000));
        double ssd_movement_estimated_time = ssd_safe_time / 2;
        double delta_time = delta_parameter * ssd_movement_estimated_time; //TODO: Very Important, we need to Prune this
        ssd_safe_time += delta_time;


        double cpu_safe_time = 2*(system_latency_us + curr_tensor->size_in_byte / (double)(CPU_PCIe_bandwidth_GBps*1024*1024*1024/1000000));
        double cpu_movement_estimated_time = cpu_safe_time / 2;
        double delta_cpu_time = delta_parameter * cpu_movement_estimated_time; //TODO: Very Important, we need to Prune this
        cpu_safe_time += delta_cpu_time;

        if (cold_medium_line < cpu_safe_time)
        {
            cold_medium_line = cpu_safe_time;
        }

        if (medium_hot_line < ssd_safe_time)
        {
            medium_hot_line = ssd_safe_time;
        }
        
        
        if (!curr_tensor->is_global_weight)
        {
            if (curr_tensor->live_interval[1]==-1)
            {
                for (int k = 0; k < kernel_num; k++)
                {
                    if (k!=curr_tensor->live_interval[0])
                    {
                        EvictionGuide_Table[k].entry[curr_tensor] = Eviction_P::Dead;
                    }
                    else{
                        EvictionGuide_Table[k].entry[curr_tensor] = Eviction_P::Cold;
                        EvictionGuide_Table[k].absolute_time_entry[curr_tensor] = 0;
                    }
                }
                
            }
            else{

                for (int j = 0; j < curr_tensor->live_interval[0]; j++)
                {
                    EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Dead;
                }

                std::unordered_set<int> is_kernel_assigned;

                if (curr_tensor->hidding_intervals.size()!=0)
                {
                    for (int interval_index = 0; interval_index < curr_tensor->hidding_intervals.size(); interval_index++)
                    {
                        for (int j = curr_tensor->hidding_intervals[interval_index]->kernelLevel_interval[0]; j < curr_tensor->hidding_intervals[interval_index]->kernelLevel_interval[1]; j++){

                            double delta_time = kernel_time_table[curr_tensor->hidding_intervals[interval_index]->kernelLevel_interval[1]] - kernel_time_table[j+1];
                            Assert(delta_time >= 0);
                            if (delta_time > medium_hot_line)
                            {
                                EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Hot;
                                EvictionGuide_Table[j].absolute_time_entry[curr_tensor] = delta_time;
                            }
                            else if (delta_time < medium_hot_line && delta_time >= cold_medium_line)
                            {
                                EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Medium;
                                EvictionGuide_Table[j].absolute_time_entry[curr_tensor] = delta_time;
                            }
                            else
                            {
                                EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Cold;
                                EvictionGuide_Table[j].absolute_time_entry[curr_tensor] = delta_time;
                            }
                            is_kernel_assigned.insert(j);
                        }
                    }
                }
                

                for (int j = curr_tensor->live_interval[0]; j < curr_tensor->live_interval[1]; j++)
                {
                    if (is_kernel_assigned.find(j)==is_kernel_assigned.end())
                    {
                        EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Cold;
                        EvictionGuide_Table[j].absolute_time_entry[curr_tensor] = 0;
                    }
                }

                for (int j = curr_tensor->live_interval[1]; j < kernel_num; j++)
                {
                    EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Dead;
                }
            }
        }
        else
        {
            int current_kernel_index;
            std::vector<bool> is_kernel_assigned;  //Collect which kernel has been assigned
            is_kernel_assigned.resize(kernel_num);
            for (int j = 0; j < kernel_num; j++)
            {
                is_kernel_assigned[j] = false;
            }
             
            for (int j = 0; j < curr_tensor->hidding_intervals.size(); j++)
            {
                // j is the interval index
                if (!curr_tensor->hidding_intervals[j]->is_looped) // not looped
                {
                    for (current_kernel_index = curr_tensor->hidding_intervals[j]->kernelLevel_interval[0]; current_kernel_index < curr_tensor->hidding_intervals[j]->kernelLevel_interval[1]; current_kernel_index++)
                    {
                        
                        double delta_time = kernel_time_table[curr_tensor->hidding_intervals[j]->kernelLevel_interval[1]] - kernel_time_table[current_kernel_index+1];
                        Assert(delta_time >= 0);
                        if (delta_time > medium_hot_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Hot;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else if (delta_time < medium_hot_line && delta_time >= cold_medium_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Medium;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Cold;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        is_kernel_assigned[current_kernel_index] = true;
                    }
                    
                }
                else  // This interval is looped
                {
                    for (current_kernel_index = curr_tensor->hidding_intervals[j]->kernelLevel_interval[0]; current_kernel_index < kernel_num; current_kernel_index++)
                    {
                        
                        double delta_time = kernel_time_table[kernel_num] - kernel_time_table[current_kernel_index+1] + kernel_time_table[curr_tensor->hidding_intervals[j]->kernelLevel_interval[1]] - kernel_time_table[0];
                        Assert(delta_time >= 0);
                        if (delta_time > medium_hot_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Hot;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else if (delta_time < medium_hot_line && delta_time >= cold_medium_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Medium;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Cold;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        is_kernel_assigned[current_kernel_index] = true;
                    }

                    for (current_kernel_index = 0; current_kernel_index < curr_tensor->hidding_intervals[j]->kernelLevel_interval[1]; current_kernel_index++)
                    {
                        
                        double delta_time = kernel_time_table[curr_tensor->hidding_intervals[j]->kernelLevel_interval[1]] - kernel_time_table[current_kernel_index+1];
                        Assert(delta_time >= 0);
                        if (delta_time > medium_hot_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Hot;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else if (delta_time < medium_hot_line && delta_time >= cold_medium_line)
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Medium;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        else
                        {
                            EvictionGuide_Table[current_kernel_index].entry[curr_tensor] = Eviction_P::Cold;
                            EvictionGuide_Table[current_kernel_index].absolute_time_entry[curr_tensor] = delta_time;
                        }
                        is_kernel_assigned[current_kernel_index] = true;
                    }
                }
                
            }

            for (int j = 0; j < kernel_num; j++)
            {
                if (!is_kernel_assigned[j])
                {
                    EvictionGuide_Table[j].entry[curr_tensor] = Eviction_P::Cold;
                    EvictionGuide_Table[j].absolute_time_entry[curr_tensor] = 0;
                }
            }
        }
    }
    
}


void print_eviction_guide_table(){
    for (int i = 0; i < tensor_list.size(); i++)
    {
        std::cout<<"@@@@@@@@@@@@@@@@@@@@@@@@@@@@"<<std::endl;
        Tensor* curr_tensor = tensor_list[i];
        curr_tensor->print();
        for (int j = 0; j < kernel_list.size(); j++)
        {
            std::cout<<"Kernel id: "<<j<<": "<<print_eviction_array[EvictionGuide_Table[j].entry[curr_tensor]]<<", absolute delta time is :" << EvictionGuide_Table[j].absolute_time_entry[curr_tensor]<<std::endl;
        }
        
    }
    
}


bool check_GPU_OK(long target_line){
    bool status = true;
    for (int i = 0; i < kernel_list.size(); i++)
    {
        if (GPU_resident_memory_estimation[i] > target_line)
        {
            status = false;
            break;
        }
    }
    return status;
}


bool check_GPU_OK_interval(long target_line, int start, int end){
    bool status = true;
    if (end < start)
    {
        end += kernel_list.size();
    }
    
    for (int i = start; i < end; i++)
    {
        if (GPU_resident_memory_estimation[i % kernel_list.size()] > target_line)
        {
            status = false;
            break;
        }
    }
    return status;
}


bool check_CPU_OK(long target_line){
    bool status = true;
    for (int i = 0; i < kernel_list.size(); i++)
    {
        if (CPU_resident_memory_estimation[i] > target_line)
        {
            status = false;
            break;
        }
    }
    return status;
}

bool check_CPU_OK_interval(long target_line, int start, int end){
    bool status = true;
    if (end < start)
    {
        end += kernel_list.size();
    }
    
    for (int i = start; i < end; i++)
    {
        if (CPU_resident_memory_estimation[i % kernel_list.size()] > target_line)
        {
            status = false;
            break;
        }
    }
    return status;
}


void CPU_add_update_interval(long tensor_size, int start, int end){
    if (end < start)
    {
        end += kernel_list.size();
    }
    
    for (int i = start; i < end; i++)
    {
       CPU_resident_memory_estimation[i % kernel_list.size()] += tensor_size;
    }
}


struct kernel_BW_buffer{
    double capacity;
    double estimation;
    bool full;
};

vector<kernel_BW_buffer> ssd2gpu_BW_estimation;  //left is the capacity, right is the current utilization
vector<kernel_BW_buffer> pcie2gpu_BW_estimation;
vector<kernel_BW_buffer> gpu2ssd_BW_estimation;  
vector<kernel_BW_buffer> gpu2pcie_BW_estimation;



//For non-UVM
int gpu2ssd_BWgiveIndx(long tensor_size, int offload_index, bool is_looped, int period_end_index, int& offload_index_out){
    long rest_size = tensor_size;
    int curr_index = offload_index;
    int kernel_num = kernel_list.size();
    bool continuity = false;

    if (!is_looped)
    {
        // Assert(period_end_index >= offload_index);
        while (rest_size > 0 && curr_index < period_end_index)
        {
            if (!gpu2ssd_BW_estimation[curr_index].full && !continuity)
            {
                continuity = true;
                offload_index_out = curr_index;
                rest_size -= (gpu2ssd_BW_estimation[curr_index].capacity - gpu2ssd_BW_estimation[curr_index].estimation);
            }
            else if (continuity && gpu2ssd_BW_estimation[curr_index].estimation > 1)
            {
                continuity = false;
                if (rest_size - (gpu2ssd_BW_estimation[curr_index].capacity - gpu2ssd_BW_estimation[curr_index].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && gpu2ssd_BW_estimation[curr_index].estimation <= 1)
            {
                rest_size -= (gpu2ssd_BW_estimation[curr_index].capacity - gpu2ssd_BW_estimation[curr_index].estimation);
            }

            if (rest_size <= 0 || curr_index == period_end_index-1)
            {
                break;
            }
            
            curr_index++;
        }
    }
    else
    {
        // Assert(period_end_index <= offload_index);
        while (rest_size > 0 && (curr_index < period_end_index + kernel_list.size()))
        {
            // if (!gpu2ssd_BW_estimation[curr_index % kernel_num].full)
            // {
            //     rest_size -= (gpu2ssd_BW_estimation[curr_index % kernel_num].capacity - gpu2ssd_BW_estimation[curr_index % kernel_num].estimation);
                
            // }

            if (!gpu2ssd_BW_estimation[curr_index % kernel_num].full && !continuity)
            {
                continuity = true;
                offload_index_out = curr_index;
                rest_size -= (gpu2ssd_BW_estimation[curr_index % kernel_num].capacity - gpu2ssd_BW_estimation[curr_index % kernel_num].estimation);
            }
            else if (continuity && gpu2ssd_BW_estimation[curr_index % kernel_num].estimation > 1)
            {
                continuity = false;
                if (rest_size - (gpu2ssd_BW_estimation[curr_index % kernel_num].capacity - gpu2ssd_BW_estimation[curr_index % kernel_num].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && gpu2ssd_BW_estimation[curr_index % kernel_num].estimation <= 1)
            {
                rest_size -= (gpu2ssd_BW_estimation[curr_index % kernel_num].capacity - gpu2ssd_BW_estimation[curr_index % kernel_num].estimation);
            }


            if (rest_size <= 0 || curr_index % kernel_num == period_end_index-1)
            {
                break;
            }
            
            curr_index++;
        }
    }

    return (curr_index+1);
}



//Return -1 if SSD BW is crowded
int gpu2ssd_BWcheck(long tensor_size, int offload_index, int ideal_finish_index, bool is_looped, int period_end_index){
    long rest_size = tensor_size;
    bool traffic_ok = false;
    Assert(offload_index<=ideal_finish_index);
    int kernel_num = kernel_list.size();

    for (int i = offload_index; i < ideal_finish_index; i++)
    {
        if (!gpu2ssd_BW_estimation[i % kernel_num].full)
        {
            traffic_ok = true;
            break;
        }
    }
    if (!traffic_ok)
    {
        return -1;
    }
    else
    {
        int curr_index = offload_index;
        int offload_index_out = -1;
        curr_index = gpu2ssd_BWgiveIndx(tensor_size, offload_index, is_looped, period_end_index, offload_index_out);
        return curr_index;
    }
}




//Used for UVM
int gpu2ssd_BWgiveIndx_half(long tensor_size, int offload_index){
    long rest_size = tensor_size / 2;
    int curr_index = offload_index;
    while (rest_size > 0 && curr_index < kernel_list.size())
    {
        if (!gpu2ssd_BW_estimation[curr_index].full)
        {
            rest_size -= (gpu2ssd_BW_estimation[curr_index].capacity - gpu2ssd_BW_estimation[curr_index].estimation);
            
        }
        if (rest_size <= 0 || curr_index == kernel_list.size()-1)
        {
            break;
        }
        
        curr_index++;
    }
    return curr_index;
}




void gpu2ssd_BWsim(long tensor_size, int offload_index){
    long rest_size = tensor_size;
    int curr_index = offload_index;
    int kernel_numm = kernel_list.size();

    while (rest_size > 0 && curr_index < offload_index + kernel_numm)
    {
        if (!gpu2ssd_BW_estimation[curr_index % kernel_numm].full)
        {
            //
            if (rest_size + gpu2ssd_BW_estimation[curr_index % kernel_numm].estimation > gpu2ssd_BW_estimation[curr_index % kernel_numm].capacity)
            {
                rest_size -= (gpu2ssd_BW_estimation[curr_index % kernel_numm].capacity - gpu2ssd_BW_estimation[curr_index % kernel_numm].estimation);
                gpu2pcie_BW_estimation[curr_index % kernel_numm].estimation += (gpu2ssd_BW_estimation[curr_index % kernel_numm].capacity - gpu2ssd_BW_estimation[curr_index % kernel_numm].estimation);
                if (gpu2pcie_BW_estimation[curr_index % kernel_numm].estimation >= gpu2pcie_BW_estimation[curr_index % kernel_numm].capacity)
                {
                    gpu2pcie_BW_estimation[curr_index % kernel_numm].full = true;
                }
                
                gpu2ssd_BW_estimation[curr_index % kernel_numm].estimation = gpu2ssd_BW_estimation[curr_index % kernel_numm].capacity;
                gpu2ssd_BW_estimation[curr_index % kernel_numm].full = true;
            }
            else
            {
                gpu2ssd_BW_estimation[curr_index % kernel_numm].estimation += rest_size;
                gpu2pcie_BW_estimation[curr_index % kernel_numm].estimation += rest_size;
                if (gpu2pcie_BW_estimation[curr_index % kernel_numm].estimation >= gpu2pcie_BW_estimation[curr_index % kernel_numm].capacity)
                {
                    gpu2pcie_BW_estimation[curr_index % kernel_numm].full = true;
                }
                rest_size = -1;
            }                
        }
        if (rest_size <= 0 || curr_index == offload_index + kernel_numm -1)
        {
            break;
        }
        
        curr_index++;
    }
}


int gpu2pcie_BWgiveIndx(long tensor_size, int offload_index, bool is_looped, int period_end_index, int& offload_index_out){
    long rest_size = tensor_size;
    int curr_index = offload_index;
    int kernel_num = kernel_list.size();
    bool continuity = false;

    if (!is_looped)
    {
        // Assert(period_end_index >= offload_index);
        while (rest_size > 0 && curr_index < period_end_index)
        {
            if (!gpu2pcie_BW_estimation[curr_index].full && !continuity)
            {
                continuity = true;
                offload_index_out = curr_index;
                rest_size -= (gpu2pcie_BW_estimation[curr_index].capacity - gpu2pcie_BW_estimation[curr_index].estimation);
            }
            else if (continuity && gpu2pcie_BW_estimation[curr_index].estimation > 1)
            {
                continuity = false;
                if (rest_size - (gpu2pcie_BW_estimation[curr_index].capacity - gpu2pcie_BW_estimation[curr_index].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && gpu2pcie_BW_estimation[curr_index].estimation <= 1)
            {
                rest_size -= (gpu2pcie_BW_estimation[curr_index].capacity - gpu2pcie_BW_estimation[curr_index].estimation);
            }

            if (rest_size <= 0 || curr_index == period_end_index-1)
            {
                break;
            }
            
            curr_index++;
        }
    }
    else
    {
        // Assert(period_end_index <= offload_index);
        while (rest_size > 0 && (curr_index < period_end_index + kernel_list.size()))
        {
            // if (!gpu2pcie_BW_estimation[curr_index % kernel_num].full)
            // {
            //     rest_size -= (gpu2pcie_BW_estimation[curr_index % kernel_num].capacity - gpu2pcie_BW_estimation[curr_index % kernel_num].estimation);
                
            // }

            if (!gpu2pcie_BW_estimation[curr_index % kernel_num].full && !continuity)
            {
                continuity = true;
                offload_index_out = curr_index;
                rest_size -= (gpu2pcie_BW_estimation[curr_index % kernel_num].capacity - gpu2pcie_BW_estimation[curr_index % kernel_num].estimation);
            }
            else if (continuity && gpu2pcie_BW_estimation[curr_index % kernel_num].estimation > 1)
            {
                continuity = false;
                if (rest_size - (gpu2pcie_BW_estimation[curr_index % kernel_num].capacity - gpu2pcie_BW_estimation[curr_index % kernel_num].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && gpu2pcie_BW_estimation[curr_index % kernel_num].estimation <= 1)
            {
                rest_size -= (gpu2pcie_BW_estimation[curr_index % kernel_num].capacity - gpu2pcie_BW_estimation[curr_index % kernel_num].estimation);
            }


            if (rest_size <= 0 || curr_index % kernel_num == period_end_index-1)
            {
                break;
            }
            
            curr_index++;
        }
    }

    return (curr_index+1);
}



void gpu2pcie_BWsim(long tensor_size, int offload_index){
    long rest_size = tensor_size;
    int curr_index = offload_index;
    int kernel_numm = kernel_list.size();
    while (rest_size > 0 && curr_index < offload_index + kernel_numm)
    {
        if (!gpu2pcie_BW_estimation[curr_index%kernel_numm].full)
        {
            //
            if (rest_size + gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation > gpu2pcie_BW_estimation[curr_index%kernel_numm].capacity)
            {
                rest_size -= (gpu2pcie_BW_estimation[curr_index%kernel_numm].capacity - gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation);
                gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation = gpu2pcie_BW_estimation[curr_index%kernel_numm].capacity;
                gpu2pcie_BW_estimation[curr_index%kernel_numm].full = true;
                
                gpu2ssd_BW_estimation[curr_index%kernel_numm].estimation = gpu2ssd_BW_estimation[curr_index%kernel_numm].capacity;
                gpu2ssd_BW_estimation[curr_index%kernel_numm].full = true;
            }
            else
            {
                gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation += rest_size;
                if (gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation >= (gpu2pcie_BW_estimation[curr_index%kernel_numm].capacity - gpu2ssd_BW_estimation[curr_index%kernel_numm].capacity))
                {
                    gpu2ssd_BW_estimation[curr_index%kernel_numm].estimation += gpu2pcie_BW_estimation[curr_index%kernel_numm].estimation - (gpu2pcie_BW_estimation[curr_index%kernel_numm].capacity - gpu2ssd_BW_estimation[curr_index%kernel_numm].capacity);
                    if (gpu2ssd_BW_estimation[curr_index%kernel_numm].estimation >= gpu2ssd_BW_estimation[curr_index%kernel_numm].capacity)
                    {
                        gpu2ssd_BW_estimation[curr_index%kernel_numm].estimation = gpu2ssd_BW_estimation[curr_index%kernel_numm].capacity;
                        gpu2ssd_BW_estimation[curr_index%kernel_numm].full = true;
                    }
                }
                rest_size = -1;
            }                
        }
        if (rest_size <= 0 || curr_index == offload_index + kernel_numm -1)
        {
            break;
        }
        
        curr_index++;
    }
}



//Use for non-UVM
int ssd2gpu_BWgiveIndx(long tensor_size, int needed_index, bool is_looped, int period_start_index, int& prefetch_finish_index_out){
    long rest_size = tensor_size;
    int curr_index = needed_index - 1;
    int kernel_num = kernel_list.size();
    bool continuity = false;

    if (!is_looped)
    {
        while (rest_size > 0 && curr_index >= period_start_index)
        {
            if (!ssd2gpu_BW_estimation[curr_index].full && !continuity)
            {
                continuity = true;
                prefetch_finish_index_out = curr_index;
                rest_size -= (ssd2gpu_BW_estimation[curr_index].capacity - ssd2gpu_BW_estimation[curr_index].estimation);
            }
            else if (continuity && ssd2gpu_BW_estimation[curr_index].estimation > 1.0)
            {
                continuity = false;
                if (rest_size - (ssd2gpu_BW_estimation[curr_index].capacity - ssd2gpu_BW_estimation[curr_index].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && ssd2gpu_BW_estimation[curr_index].estimation <= 1.0)
            {
                rest_size -= (ssd2gpu_BW_estimation[curr_index].capacity - ssd2gpu_BW_estimation[curr_index].estimation);
                
            }


            if (rest_size <= 0 || curr_index == period_start_index)
            {
                break;
            }
            
            curr_index--;
        }
    }
    else
    {
        curr_index += kernel_num;
        while (rest_size > 0 && curr_index >= period_start_index)
        {
            if (!ssd2gpu_BW_estimation[curr_index % kernel_num].full && !continuity)
            {
                continuity = true;
                prefetch_finish_index_out = curr_index;
                rest_size -= (ssd2gpu_BW_estimation[curr_index % kernel_num].capacity - ssd2gpu_BW_estimation[curr_index % kernel_num].estimation);
            }
            else if (continuity && ssd2gpu_BW_estimation[curr_index % kernel_num].estimation > 1)
            {
                continuity = false;
                if (rest_size - (ssd2gpu_BW_estimation[curr_index % kernel_num].capacity - ssd2gpu_BW_estimation[curr_index % kernel_num].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && ssd2gpu_BW_estimation[curr_index % kernel_num].estimation <= 1)
            {
                rest_size -= (ssd2gpu_BW_estimation[curr_index % kernel_num].capacity - ssd2gpu_BW_estimation[curr_index % kernel_num].estimation);
                
            }

            if (rest_size <= 0 || curr_index == period_start_index)
            {
                break;
            }
            
            curr_index--;
        }
    }
    
    return curr_index;
}


//Use for UVM
int ssd2gpu_BWgiveIndx_half(long tensor_size, int needed_index){
    long rest_size = tensor_size / 2;
    int curr_index = needed_index - 1;
    while (rest_size > 0 && curr_index >= 0)
    {
        if (!ssd2gpu_BW_estimation[curr_index].full)
        {
            rest_size -= (ssd2gpu_BW_estimation[curr_index].capacity - ssd2gpu_BW_estimation[curr_index].estimation);
            
        }
        if (rest_size <= 0 || curr_index == 0)
        {
            break;
        }
        
        curr_index--;
    }
    return curr_index;
}

//Now it has no boundary check
void ssd2gpu_BWsim(long tensor_size, int fetch_index){
    long rest_size = tensor_size;
    int curr_index = fetch_index;
    while (rest_size > 0 && curr_index < fetch_index + kernel_list.size())
    {
        if (!ssd2gpu_BW_estimation[curr_index % kernel_list.size()].full)
        {
            //
            if (rest_size + ssd2gpu_BW_estimation[curr_index % kernel_list.size()].estimation > ssd2gpu_BW_estimation[curr_index % kernel_list.size()].capacity)
            {
                rest_size -= (ssd2gpu_BW_estimation[curr_index % kernel_list.size()].capacity - ssd2gpu_BW_estimation[curr_index % kernel_list.size()].estimation);
                pcie2gpu_BW_estimation[curr_index % kernel_list.size()].estimation += (ssd2gpu_BW_estimation[curr_index % kernel_list.size()].capacity - ssd2gpu_BW_estimation[curr_index % kernel_list.size()].estimation);
                if (pcie2gpu_BW_estimation[curr_index % kernel_list.size()].estimation >= pcie2gpu_BW_estimation[curr_index % kernel_list.size()].capacity)
                {
                    pcie2gpu_BW_estimation[curr_index % kernel_list.size()].full = true;
                }
                
                ssd2gpu_BW_estimation[curr_index % kernel_list.size()].estimation = ssd2gpu_BW_estimation[curr_index % kernel_list.size()].capacity;
                ssd2gpu_BW_estimation[curr_index % kernel_list.size()].full = true;
            }
            else
            {
                ssd2gpu_BW_estimation[curr_index % kernel_list.size()].estimation += rest_size;
                pcie2gpu_BW_estimation[curr_index % kernel_list.size()].estimation += rest_size;
                if (pcie2gpu_BW_estimation[curr_index % kernel_list.size()].estimation >= pcie2gpu_BW_estimation[curr_index % kernel_list.size()].capacity)
                {
                    pcie2gpu_BW_estimation[curr_index % kernel_list.size()].full = true;
                }
                rest_size = -1;
            }                
        }
        if (rest_size <= 0 || curr_index == fetch_index + kernel_list.size() -1)
        {
            break;
        }
        
        curr_index++;
    }
}


int pcie2gpu_BWgiveIndx(long tensor_size, int needed_index, bool is_looped, int period_start_index, int& prefetch_finish_index_out){
    long rest_size = tensor_size;
    int curr_index = needed_index - 1;
    int kernel_num = kernel_list.size();
    bool continuity = false;

    if (!is_looped)
    {
        while (rest_size > 0 && curr_index >= period_start_index)
        {
            if (!pcie2gpu_BW_estimation[curr_index].full && !continuity)
            {
                continuity = true;
                prefetch_finish_index_out = curr_index;
                rest_size -= (pcie2gpu_BW_estimation[curr_index].capacity - pcie2gpu_BW_estimation[curr_index].estimation);
            }
            else if (continuity && pcie2gpu_BW_estimation[curr_index].estimation > 1)
            {
                continuity = false;
                if (rest_size - (pcie2gpu_BW_estimation[curr_index].capacity - pcie2gpu_BW_estimation[curr_index].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && pcie2gpu_BW_estimation[curr_index].estimation <= 1)
            {
                rest_size -= (pcie2gpu_BW_estimation[curr_index].capacity - pcie2gpu_BW_estimation[curr_index].estimation);
                
            }


            if (rest_size <= 0 || curr_index == period_start_index)
            {
                break;
            }
            
            curr_index--;
        }
    }
    else
    {
        curr_index += kernel_num;
        while (rest_size > 0 && curr_index >= period_start_index)
        {
            if (!pcie2gpu_BW_estimation[curr_index % kernel_num].full && !continuity)
            {
                continuity = true;
                prefetch_finish_index_out = curr_index;
                rest_size -= (pcie2gpu_BW_estimation[curr_index % kernel_num].capacity - pcie2gpu_BW_estimation[curr_index % kernel_num].estimation);
            }
            else if (continuity && pcie2gpu_BW_estimation[curr_index % kernel_num].estimation > 1)
            {
                continuity = false;
                if (rest_size - (pcie2gpu_BW_estimation[curr_index % kernel_num].capacity - pcie2gpu_BW_estimation[curr_index % kernel_num].estimation) > 0)
                {
                    rest_size = tensor_size;
                }
                else
                {
                    rest_size = -1;
                }
            }
            else if (continuity && pcie2gpu_BW_estimation[curr_index % kernel_num].estimation <= 1)
            {
                rest_size -= (pcie2gpu_BW_estimation[curr_index % kernel_num].capacity - pcie2gpu_BW_estimation[curr_index % kernel_num].estimation);
                
            }

            if (rest_size <= 0 || curr_index == period_start_index)
            {
                break;
            }
            
            curr_index--;
        }
    }
    
    return curr_index;
}


void pcie2gpu_BWsim(long tensor_size, int fetch_index){
    long rest_size = tensor_size;
    int curr_index = fetch_index;
    int kernel_numm = kernel_list.size();

    while (rest_size > 0 && curr_index <= fetch_index + kernel_numm)
    {
        if (!pcie2gpu_BW_estimation[curr_index % kernel_numm].full)
        {
            //
            if (rest_size + pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation > pcie2gpu_BW_estimation[curr_index % kernel_numm].capacity)
            {
                rest_size -= (pcie2gpu_BW_estimation[curr_index % kernel_numm].capacity - pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation);
                pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation = pcie2gpu_BW_estimation[curr_index % kernel_numm].capacity;
                pcie2gpu_BW_estimation[curr_index % kernel_numm].full = true;
                
                ssd2gpu_BW_estimation[curr_index % kernel_numm].estimation = ssd2gpu_BW_estimation[curr_index % kernel_numm].capacity;
                ssd2gpu_BW_estimation[curr_index % kernel_numm].full = true;
            }
            else
            {
                pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation += rest_size;
                if (pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation >= (pcie2gpu_BW_estimation[curr_index % kernel_numm].capacity - ssd2gpu_BW_estimation[curr_index % kernel_numm].capacity))
                {
                    ssd2gpu_BW_estimation[curr_index % kernel_numm].estimation += pcie2gpu_BW_estimation[curr_index % kernel_numm].estimation - (pcie2gpu_BW_estimation[curr_index % kernel_numm].capacity - ssd2gpu_BW_estimation[curr_index % kernel_numm].capacity);
                    if (ssd2gpu_BW_estimation[curr_index % kernel_numm].estimation >= ssd2gpu_BW_estimation[curr_index % kernel_numm].capacity)
                    {
                        ssd2gpu_BW_estimation[curr_index % kernel_numm].estimation = ssd2gpu_BW_estimation[curr_index % kernel_numm].capacity;
                        ssd2gpu_BW_estimation[curr_index % kernel_numm].full = true;
                    }
                }
                rest_size = -1;
            }                
        }
        if (rest_size <= 0 || curr_index == fetch_index + kernel_numm-1)
        {
            break;
        }
        
        curr_index++;
    }
}


void print_BW_estimations(){
    // std::cout<<"----------------------------------------------------"<<std::endl;
    // std::cout<<"SSD2GPU BW estimation:"<<std::endl;
    // for (int i = 0; i < kernel_list.size(); i++)
    // {
    //     std::cout<<"KernelID: "<<i<<"*** Status(full): "<<ssd2gpu_BW_estimation[i].full<<", Estimation: "<<ssd2gpu_BW_estimation[i].estimation<<", Capacity: "<<ssd2gpu_BW_estimation[i].capacity<<";"<<std::endl;
    // }
    // std::cout<<"----------------------------------------------------"<<std::endl;
    // std::cout<<"GPU2SSD BW estimation:"<<std::endl;
    // for (int i = 0; i < kernel_list.size(); i++)
    // {
    //     std::cout<<"KernelID: "<<i<<"*** Status(full): "<<gpu2ssd_BW_estimation[i].full<<", Estimation: "<<gpu2ssd_BW_estimation[i].estimation<<", Capacity: "<<gpu2ssd_BW_estimation[i].capacity<<";"<<std::endl;
    // }
    // std::cout<<"----------------------------------------------------"<<std::endl;
    // std::cout<<"GPU2PCIE BW estimation:"<<std::endl;
    // for (int i = 0; i < kernel_list.size(); i++)
    // {
    //     std::cout<<"KernelID: "<<i<<"*** Status(full): "<<gpu2pcie_BW_estimation[i].full<<", Estimation: "<<gpu2pcie_BW_estimation[i].estimation<<", Capacity: "<<gpu2pcie_BW_estimation[i].capacity<<";"<<std::endl;
    // }
    // std::cout<<"----------------------------------------------------"<<std::endl;
    // std::cout<<"PCIE2GPU BW estimation:"<<std::endl;
    // for (int i = 0; i < kernel_list.size(); i++)
    // {
    //     std::cout<<"KernelID: "<<i<<"*** Status(full): "<<pcie2gpu_BW_estimation[i].full<<", Estimation: "<<pcie2gpu_BW_estimation[i].estimation<<", Capacity: "<<pcie2gpu_BW_estimation[i].capacity<<";"<<std::endl;
    // }

    // std::cout<<"SSD2GPU BW estimation:"<<std::endl;


    long T = 1000000; //1ms
    long current_t = 0;
    double running_avg = 0;
    long counter = 0;

    std::cout<<"time_tick = "<<T<<std::endl;

    std::cout<<"ssd2gpu_bw = [";
    for (int i = 0; i < kernel_list.size(); i++)
    {
        long t_added = current_t + kernel_list[i].execution_cycles / (GPU_frequency_GHz);
        int current_kernel_util = int(ssd2gpu_BW_estimation[i].estimation>0);

        if (t_added > T)
        {
            //First T
            running_avg = (running_avg * current_t + current_kernel_util * (T - current_t))/T;
            std::cout<<int(running_avg>0.5)<<",";
            current_t = 0;
            running_avg = 0;
            t_added = t_added - T;
            //The following several T
            int num_T = t_added / T;
            int rest = t_added % T;
            for (int j = 0; j < num_T; j++)
            {
                std::cout<<current_kernel_util<<",";
                counter++;
            }
            current_t = rest;
            running_avg = current_kernel_util;
        }
        else
        {
            running_avg = (running_avg * current_t + current_kernel_util * (t_added - current_t)) / t_added;
            current_t = t_added;
        }
    }
    //TODO: check!
    std::cout<<"]"<<std::endl;

    std::cout<<"time_counter = "<<counter<<std::endl;

    current_t = 0;
    running_avg = 0;
    counter = 0;

    // std::cout<<"----------------------------------------------------"<<std::endl;
    // std::cout<<"GPU2SSD BW estimation:"<<std::endl;
    std::cout<<"gpu2ssd_bw = [";
    for (int i = 0; i < kernel_list.size(); i++)
    {
        long t_added = current_t + kernel_list[i].execution_cycles / (GPU_frequency_GHz);
        int current_kernel_util = int(gpu2ssd_BW_estimation[i].estimation>0);

        if (t_added > T)
        {
            //First T
            running_avg = (running_avg * current_t + current_kernel_util * (T - current_t))/T;
            std::cout<<int(running_avg>0.5)<<",";
            current_t = 0;
            running_avg = 0;
            t_added = t_added - T;
            //The following several T
            int num_T = t_added / T;
            int rest = t_added % T;
            for (int j = 0; j < num_T; j++)
            {
                std::cout<<current_kernel_util<<",";
                counter++;
            }
            current_t = rest;
            running_avg = current_kernel_util;
        }
        else
        {
            running_avg = (running_avg * current_t + current_kernel_util * (t_added - current_t)) / t_added;
            current_t = t_added;
        }
    }
    //TODO: check!
    std::cout<<"]"<<std::endl;

    std::cout<<"time_counter = "<<counter<<std::endl;
    
}


int scheduling_offload_flashneuron(){
    // First Calculate the total space for un-offloading tensors
    long GPU_line = (long)(GPU_memory_size_GB * 1024 * 1024 *1024);
    long global_tensor_size = memory_offset_weights;

    long max_num_pages = 0;
    for (auto it = kernel_list.begin(); it != kernel_list.end(); ++it) {
        CUDAKernel *current_kernel = &(*it);
        vector<Tensor *> required_tensors;
        current_kernel->getRequiredTensors(required_tensors);
        long num_pages = 0;
        for (Tensor *tensor : required_tensors) {
            num_pages += (tensor->size_in_byte / PAGE_SIZE);
        }
        if (num_pages > max_num_pages) {
            max_num_pages = num_pages;
        }
    }

    if (borden == 184 && is_transformer == 1 && kernel_list[0].parent_op->N==1024)
    {
        GPU_line = (long)(GPU_line*0.993); //For supporting VIT-1024, if don't do so it's possible the original FlashNeuron algorithm will crash
    }
    

    GPU_line = GPU_line - global_tensor_size - max_num_pages*PAGE_SIZE;

    if (GPU_line <= 0)
    {
        std::cout<<"No enough space for un-offloading tensors!"<<std::endl;
        return 1;
    }

    // Count the entire "offloadable" size in forward propogation
    int make_error_index = -1;
    for (int i = 0; i < kernel_list.size(); i++)
    {
        if (kernel_list[i].type==CUDAKernelType::makeLoss)
        {
            make_error_index = i;
            break;
        }
    }
    Assert(make_error_index!=-1);

    long total_offloadable_size = 0;
    vector<Tensor*> offloadable_tensors;
    for (int i = 0; i < make_error_index; i++)
    {
        CUDAKernel* current_kernel = &kernel_list[i];
        unordered_set<Tensor *> required_tensors;
        current_kernel->getRequiredTensors(required_tensors);
        for (const auto& tensor_j: required_tensors)
        {
            if (!tensor_j->is_global_weight && tensor_j->live_interval[1]!=-1 && tensor_j->live_interval[1] > make_error_index)
            {
                Assert(tensor_j!=current_kernel->workspace);
                // Assert(tensor_j!=current_kernel->parent_layer->weight);
                bool find = false;
                for (int l = 0; l < offloadable_tensors.size(); l++)
                {
                    if (offloadable_tensors[l]==tensor_j)
                    {
                        find = true;
                        break;
                    }
                }
                
                if (!find)
                {
                    offloadable_tensors.push_back(tensor_j);
                    total_offloadable_size += tensor_j->size_in_byte;
                }
            }
        }
    }


    //Schedule offloads
    auto itr = offloadable_tensors.begin();
    while (total_offloadable_size >= GPU_line && itr != offloadable_tensors.end())
    {
        std::cout<<".";
        Tensor* curr_tensor = *itr;
        Assert(!curr_tensor->is_global_weight);
        int proper_cold_start_index = -1;
        if (curr_tensor->hidding_intervals.size()==0)
        {
            itr++;
            std::cout<<"Can not offload this :";
            curr_tensor->print();
            continue;
        }
        for (int i = 0; i < curr_tensor->hidding_intervals.size(); i++)
        {
            if(curr_tensor->hidding_intervals[i]->kernelLevel_interval[0] <= make_error_index){
                proper_cold_start_index = curr_tensor->hidding_intervals[i]->kernelLevel_interval[0];
            }
            else
            {
                break;
            }
        }
        Assert(proper_cold_start_index!=-1);
        Offload_Hint_FlashNeuron offload(proper_cold_start_index, curr_tensor);
        offload_hints_fn.push_back(offload);
        curr_tensor->f_is_choosed_to_offload = true;
        total_offloadable_size -= curr_tensor->size_in_byte;
        itr++;
    }

    if (total_offloadable_size > GPU_line)
    {
        std::cout<<"Final remaining size: "<< total_offloadable_size<< ", "<< "GPU_line: "<< GPU_line<<std::endl;
        return 1;
    }
    else
    {
        // Sort the intervals
        std::sort(offload_hints_fn.begin(), offload_hints_fn.end(), [](Offload_Hint_FlashNeuron a, Offload_Hint_FlashNeuron b){
            return (a.issued_time < b.issued_time);
        });
        std::cout<<"Final remaining size: "<< total_offloadable_size<< ", "<< "GPU_line: "<< GPU_line<<std::endl;
        return 0;
    }
}



void print_offloading_flashneuron(){
    for (int i = 0; i < offload_hints_fn.size(); i++)
    {
        Offload_Hint_FlashNeuron curr = offload_hints_fn[i];
        std::cout << "Issued Time: " << curr.issued_time << " ";
        std::cout << "Tensor: " << curr.tensor->tensor_id << " G:" << (curr.tensor->is_global_weight ? "o" : "x") << " "<<std::endl;
    }
    
}





void scheduling_prefetch(){
    if (migration_policy_str=="DEEPUM")
    {
        for (int i = 0; i < kernel_list.size(); i++)
        {
            std::vector<Tensor*> required_tensor_N;
            kernel_list[(i+prefetch_degree)%kernel_list.size()].getRequiredTensors(required_tensor_N);
            for (int j = 0; j < required_tensor_N.size(); j++)
            {
                DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, i, required_tensor_N[j]);
                movement_hints.push_back(pre_fetch);
            }
        }
        return;
    }

    
    long total_mem_size = memory_offset_intermediate + memory_offset_weights + tensor_list[0]->size_in_byte;
    kernel_num = kernel_list.size();

    long target_mem_line = (long)(GPU_memory_size_GB * 1024 * 1024 * 1024);
    long tolerant_line = target_mem_line * 0.7; 
    CPU_line = (long)(CPU_memory_line_GB * 1024 * 1024 * 1024);

    int hill_index = 0;
    long hill_mem = 0;
    

    //New: remove all CPU tensors:
    for (int i = 0; i < tensor_list.size(); i++)
    {
        if (!tensor_list[i]->is_global_weight && tensor_list[i]->live_interval[1] == -1 && tensor_list[i]->live_interval[0] == -1)
        {
            total_mem_size -= tensor_list[i]->size_in_byte;
        }
    }


     //Fill gpu memory first with all the tensors
    GPU_resident_memory_estimation.resize(kernel_num);
    for (int i = 0; i < kernel_num; i++)
    {
        GPU_resident_memory_estimation[i] = total_mem_size;
    }

    //Fill cpu memory first with 0
    CPU_resident_memory_estimation.resize(kernel_num);
    for (int i = 0; i < kernel_num; i++)
    {
        CPU_resident_memory_estimation[i] = 0;
    }

    //Initialize the BW estimation arrays
    ssd2gpu_BW_estimation.resize(kernel_num);
    pcie2gpu_BW_estimation.resize(kernel_num);
    gpu2ssd_BW_estimation.resize(kernel_num);
    gpu2pcie_BW_estimation.resize(kernel_num);
    for (int i = 0; i < kernel_num; i++)
    {
        ssd2gpu_BW_estimation[i].capacity = SSD_PCIe_bandwidth_GBps*1024*1024*1024/1000000*(kernel_time_table[i+1] - kernel_time_table[i]);
        ssd2gpu_BW_estimation[i].estimation = 0;
        ssd2gpu_BW_estimation[i].full = false;
        gpu2ssd_BW_estimation[i].capacity = SSD_PCIe_bandwidth_GBps*1024*1024*1024/1000000*(kernel_time_table[i+1] - kernel_time_table[i]);
        gpu2ssd_BW_estimation[i].estimation = 0;
        gpu2ssd_BW_estimation[i].full = false;
        pcie2gpu_BW_estimation[i].capacity = CPU_PCIe_bandwidth_GBps*1024*1024*1024/1000000*(kernel_time_table[i+1] - kernel_time_table[i]);
        pcie2gpu_BW_estimation[i].estimation = 0;
        pcie2gpu_BW_estimation[i].full = false;
        gpu2pcie_BW_estimation[i].capacity = CPU_PCIe_bandwidth_GBps*1024*1024*1024/1000000*(kernel_time_table[i+1] - kernel_time_table[i]);
        gpu2pcie_BW_estimation[i].estimation = 0;
        gpu2pcie_BW_estimation[i].full = false;
    }


    // if(migration_policy_str=="G10GDSSSD" || migration_policy_str=="G10GDSFULL"){ //For GDS-based baselines, do some loosen on the target line 
    //                                                                              //can gain best performance since without UVM the GPU mem requirement is stict
    //     loosen_parameter = 1.060606;
    //     if (borden<200 && is_transformer==1 && migration_policy_str=="G10GDSFULL")
    //     {
    //         loosen_parameter = 1.269366;
    //     }
        
    // }
    // target_mem_line = loosen_parameter * target_mem_line;    

    // //Except for A0, pre-deallocation all other tensors  #First pass - to figure out the memory pressure region
    // for (int i = 1; i < tensor_list.size(); i++)
    // {
    //     if (check_GPU_OK(target_mem_line))    //If already OK, end this loop
    //     {
    //         break;
    //     }
    //     if (!tensor_list[i]->is_global_weight && tensor_list[i]->live_interval[1] == -1 && tensor_list[i]->live_interval[0] == -1)
    //     {
    //         continue;
    //     }
        
    //     Tensor* curr_tensor = tensor_list[i];
    //     if (!curr_tensor->is_global_weight)
    //     {
    //         //First do pre-alloc
    //         int issue_index;
    //         int birth_date_index = curr_tensor->live_interval[0];
    //         double estimated_pre_alloc_time = curr_tensor->size_in_byte * GPU_malloc_uspB;
    //         double pre_alloc_start_time_precise = kernel_time_table[birth_date_index] - estimated_pre_alloc_time;
    //         if (pre_alloc_start_time_precise < 0)
    //         {
    //             //DataMovementHint pre_allo(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, 0, curr_tensor);
    //             //movement_hints.push_back(pre_allo);
    //             issue_index = 0;

    //             //minus mem
    //             for (int j = 0; j < birth_date_index; j++)
    //             {
    //                 GPU_resident_memory_estimation[j] -= curr_tensor->size_in_byte;
    //             }
    //         }
    //         else
    //         {
                    
    //             for (int j = 0; j < birth_date_index; j++)
    //             {
    //                 if (kernel_time_table[j] <= pre_alloc_start_time_precise && kernel_time_table[j+1] >= pre_alloc_start_time_precise)
    //                 {
    //                     //DataMovementHint pre_allo(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, j, curr_tensor);
    //                     //movement_hints.push_back(pre_allo);
    //                     issue_index = j;
    //                     break;
    //                 }
    //             }
                
    //             //minus mem
    //             for (int j = 0; j < issue_index; j++)
    //             {
    //                GPU_resident_memory_estimation[j] -= curr_tensor->size_in_byte;
    //             }
                    
    //         }


    //         //Second do pre-deallocation
    //         int death_index = curr_tensor->live_interval[1];
    //         if (curr_tensor->live_interval[1]==-1)
    //         {
    //             death_index = curr_tensor->live_interval[0] + 1;
    //         }

    //         //DataMovementHint pre_dallo(PageLocation::NOT_KNOWN, PageLocation::NOT_PRESENT, death_index, curr_tensor);
    //         //movement_hints.push_back(pre_dallo);

    //         //double deallo_time = curr_tensor->size_in_byte * GPU_free_uspB;
    //         double deallo_time = 0;
    //         double deallo_finish_time_precise = kernel_time_table[death_index];
    //         if (deallo_finish_time_precise < kernel_time_table[kernel_num])
    //         {
    //             int finish_index = -1;
    //             for (int j = death_index; j < kernel_num; j++)
    //             {
    //                 if (kernel_time_table[j] <= deallo_finish_time_precise && kernel_time_table[j+1] > deallo_finish_time_precise)
    //                 {
    //                     finish_index = j;
    //                     break;
    //                 }
    //             }
    //             //Assert(finish_index >= 0);
    //             if (finish_index == -1)
    //             {
    //                 finish_index = kernel_index;
    //             }
                

    //             //minus mem
    //             for (int j = finish_index; j < kernel_num; j++)
    //             {
    //                 GPU_resident_memory_estimation[j] -= curr_tensor->size_in_byte;
    //             }
    //         }
    //     }

    
    // }

    // bool is_under_pressure = false;
    // int pressure_region[2]; 
    // pressure_region[0] = -1;
    // pressure_region[1] = -1;
    // if (!check_GPU_OK(target_mem_line))    //If already OK, end this loop
    // {
    //     is_under_pressure = true;
    // }
    // if (is_under_pressure)
    // {
    //     int k_index = 0;
    //     while (k_index < kernel_num)
    //     {
    //         if (GPU_resident_memory_estimation[k_index] > target_mem_line)
    //         {
    //             pressure_region[0] = k_index;
    //             break;
    //         }
    //         k_index++;
    //     }
    //     k_index = kernel_num - 1;
    //     while (k_index >= 0)
    //     {
    //         if (GPU_resident_memory_estimation[k_index] > target_mem_line)
    //         {
    //             pressure_region[1] = k_index;
    //             break;
    //         }
    //         k_index--;
    //     }
    //     Assert(pressure_region[0] >= 0);
    //     Assert(pressure_region[1] >= 0);
    //     Assert(pressure_region[1] >= pressure_region[0]);
    // }


    //Refill gpu memory with all the tensors
    GPU_resident_memory_estimation.resize(kernel_num);
    for (int i = 0; i < kernel_num; i++)
    {
        GPU_resident_memory_estimation[i] = total_mem_size;
    }
    


    //Except for A0, pre-deallocation all other tensors - Second pass, schedule the smart migration instructions
    for (int i = 1; i < tensor_list.size(); i++)
    {
        if (check_GPU_OK(target_mem_line))    //If already OK, end this loop
        {
            break;
        }
        if (!tensor_list[i]->is_global_weight && tensor_list[i]->live_interval[1] == -1 && tensor_list[i]->live_interval[0] == -1)
        {
            continue;
        }
        
        Tensor* curr_tensor = tensor_list[i];
        if (!curr_tensor->is_global_weight)
        {
            //First do pre-alloc
            int issue_index;
            int birth_date_index = curr_tensor->live_interval[0];
            // double estimated_pre_alloc_time;
            // if (is_under_pressure && birth_date_index >= pressure_region[0] && birth_date_index <= pressure_region[1])
            // {
            //     estimated_pre_alloc_time = curr_tensor->size_in_byte * GPU_malloc_uspB;
            //     //estimated_pre_alloc_time = estimated_pre_alloc_time * (1 + delta_parameter);
            // }
            // else
            // {
            //     estimated_pre_alloc_time = curr_tensor->size_in_byte * GPU_malloc_uspB;
            // }
            
            // double pre_alloc_start_time_precise = kernel_time_table[birth_date_index] - estimated_pre_alloc_time;
            // if (pre_alloc_start_time_precise < 0)
            // {
            //     if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
            //     {
            //         DataMovementHint pre_allo(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, 0, curr_tensor);
            //         movement_hints.push_back(pre_allo);
            //     }
            //     issue_index = 0;

            // }
            // else
            // {
                    
            // for (int j = 0; j < birth_date_index; j++)
            // {
            //     if (kernel_time_table[j] <= pre_alloc_start_time_precise && kernel_time_table[j+1] > pre_alloc_start_time_precise)
            //     {
            //         if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL"){
            //             DataMovementHint pre_allo(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, j, curr_tensor);
            //             movement_hints.push_back(pre_allo);
            //         }
            //         issue_index = j;
            //         break;
            //     }
            // }
            
            //minus mem
            double tensor_size = curr_tensor->size_in_byte;
            for (int j = 0; j < birth_date_index; j++)
            {
                GPU_resident_memory_estimation[j] -= tensor_size;
            }
                    
            // }


            //Second do pre-deallocation
            int death_index = curr_tensor->live_interval[1];
            if (curr_tensor->live_interval[1]==-1)
            {
                death_index = curr_tensor->live_interval[0] + 1;
            }
            
            // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL"){
            //     DataMovementHint pre_dallo(PageLocation::NOT_KNOWN, PageLocation::NOT_PRESENT, death_index, curr_tensor);
            //     movement_hints.push_back(pre_dallo);
            // }

            //double deallo_time = curr_tensor->size_in_byte * GPU_free_uspB;
            // double deallo_time = 0;
            // double deallo_finish_time_precise = kernel_time_table[death_index];
            // if (deallo_finish_time_precise < kernel_time_table[kernel_num])
            // {
            // int finish_index = -1;
            // for (int j = death_index; j < kernel_num; j++)
            // {
            //     if (kernel_time_table[j] <= deallo_finish_time_precise && kernel_time_table[j+1] > deallo_finish_time_precise)
            //     {
            //         finish_index = j;
            //         break;
            //     }
            // }
            // //Assert(finish_index >= 0);
            // if (finish_index == -1)
            // {
            //     finish_index = kernel_index;
            // }
            

            //minus mem
            tensor_size = curr_tensor->size_in_byte;
            for (int j = death_index; j < kernel_num; j++)
            {
                GPU_resident_memory_estimation[j] -= tensor_size;
            }
            // }
        }

    
    }



    
    std::cerr<<"After pre-deallocation"<<std::endl;
    print_GPU_mem_estimation("liveness");


    for (int j = 0; j < GPU_resident_memory_estimation.size(); j++)
    {
        if (GPU_resident_memory_estimation[j] > hill_mem)
        {
            hill_mem = GPU_resident_memory_estimation[j];
            hill_index = j;
        }
    }

    //Initialize the pinned memory estimation arrays
    GPU_resident_memory_estimation_pinned.resize(kernel_num);
    for (int i = 0; i < kernel_num; i++)
    {
        long size = 0;
        std::vector<Tensor*> required_tensors_;
        kernel_list[i].getRequiredTensors(required_tensors_);
        for (int j = 0; j < required_tensors_.size(); j++)
        {
            Tensor* curr_tensor = required_tensors_[j];
            size += curr_tensor->size_in_byte;
        }
        GPU_resident_memory_estimation_pinned[i] = size;
    }

    long global_p_order_pos = 0;
    long global_p_order_neg = 0;
    

    //Fill the looped extend kernel time table      0 - 2 * kernel_num
    std::vector<double> kernel_time_table_extended;
    kernel_time_table_extended.resize(kernel_num);
    for (int j = 0; j < kernel_num; j++)
    {
        kernel_time_table_extended[j] = kernel_time_table[j];
    }
    double last_time = kernel_time_table[kernel_num];
    kernel_time_table_extended.push_back(last_time);
    for (int j = 0; j < kernel_num; j++)
    {
        last_time += (double)kernel_list[j].execution_cycles / (double)(GPU_frequency_GHz*1000);
        kernel_time_table_extended.push_back(last_time);
    }


    //Schedule the prefetches and pre-evictions, Go through all the intervals, from largest to shortest
    int cold_period_iter = -1;
    int tot_iter_num = interval_list.size();
    const int Sample = algo_speedup;

    for (int i = 0; i < tot_iter_num; i+=Sample)
    {
        if (check_GPU_OK(target_mem_line))    //If already OK, end this loop
        {
            cold_period_iter = i;
            break;
        }
        else if ( i == interval_list.size()-1)
        {
            cold_period_iter = interval_list.size();
        }


        //Calculate the area can be reduced for the sort
        for (int j = 0; j < interval_list.size(); j++)
        {
            Hidding_Interval* a = interval_list[j];
            double area_can_reduce_a = 0;
            if (a->is_offloaded || a->the_tensor->size_in_byte < 2000000)
            {
                area_can_reduce_a = 0;
                a->is_offloaded = true;
            }
            else
            {
                if (!(a->is_looped))
                {
                    int offload_mid_index;
                    int prefetch_mid_index;
                    int offload_out = -1;
                    int prefetch_out;
                    // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
                    // {
                    //     offload_mid_index = gpu2ssd_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[0]);
                    //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[1]);
                    // }
                    // else {
                    offload_mid_index = gpu2ssd_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], false, a->kernelLevel_interval[1], offload_out);
                    prefetch_mid_index = ssd2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], false, a->kernelLevel_interval[0], prefetch_out);
                    // }
                    if (((offload_out >= a->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
                    {
                        offload_mid_index = gpu2pcie_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], false, a->kernelLevel_interval[1], offload_out);
                        prefetch_mid_index = pcie2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], false, a->kernelLevel_interval[0], prefetch_out);
                    }

                    //Optimization: quick check
                    if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index] > a->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > a->GPU_mem_line
                     && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line)
                    {
                        area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index]);
                    }
                    else
                    {
                        for (int j = offload_mid_index; j < prefetch_mid_index; j++)
                        {
                            if (GPU_resident_memory_estimation[j] > a->GPU_mem_line)
                            {
                                area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
                            }
                        }
                    }
                }
                else
                {
                    int offload_mid_index;
                    int prefetch_mid_index;
                    int offload_out = -1;
                    int prefetch_out;
                    // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
                    // {
                    //     offload_mid_index = gpu2ssd_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[0]);
                    //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[1]);
                    // }
                    // else {
                    offload_mid_index = gpu2ssd_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], true, a->kernelLevel_interval[1], offload_out);
                    prefetch_mid_index = ssd2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], true, a->kernelLevel_interval[0], prefetch_out);
                    // }
                    if (((offload_out >= a->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
                    {
                        offload_mid_index = gpu2pcie_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], true, a->kernelLevel_interval[1], offload_out);
                        prefetch_mid_index = pcie2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], true, a->kernelLevel_interval[0], prefetch_out);
                    }


                    //Optimization: quick check
                    if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > a->GPU_mem_line
                        && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line)
                    {
                        area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[((prefetch_mid_index-1) % kernel_num) + 1] - kernel_time_table[offload_mid_index % kernel_num]);
                    }
                    else
                    {
                        for (int j = offload_mid_index; j < prefetch_mid_index; j++)
                        {
                            if (GPU_resident_memory_estimation[j % kernel_num] > a->GPU_mem_line)
                            {
                                area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
                            }
                        }
                    }
                }
            }
            a->area_can_reduce = area_can_reduce_a;
        }
        


        //Sort the intervals
        std::sort(interval_list.begin(), interval_list.end(), [](Hidding_Interval* a, Hidding_Interval* b){
            double area_can_reduce_a = a->area_can_reduce;
            double area_can_reduce_b = b->area_can_reduce;

            // if (a->is_offloaded || a->the_tensor->size_in_byte < 2000000)
            // {
            //     area_can_reduce_a = 0;
            //     a->is_offloaded = true;
            // }
            // else if (b->is_offloaded)
            // {
            //     area_can_reduce_a = 100000;
            // }
            // else
            // {
            //     if (!(a->is_looped))
            //     {
            //         int offload_mid_index;
            //         int prefetch_mid_index;
            //         int offload_out = -1;
            //         int prefetch_out;
            //         // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
            //         // {
            //         //     offload_mid_index = gpu2ssd_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[0]);
            //         //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[1]);
            //         // }
            //         // else {
            //         offload_mid_index = gpu2ssd_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], false, a->kernelLevel_interval[1], offload_out);
            //         prefetch_mid_index = ssd2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], false, a->kernelLevel_interval[0], prefetch_out);
            //         // }
            //         if (((offload_out >= a->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
            //         {
            //             offload_mid_index = gpu2pcie_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], false, a->kernelLevel_interval[1], offload_out);
            //             prefetch_mid_index = pcie2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], false, a->kernelLevel_interval[0], prefetch_out);
            //         }

            //         //Optimization: quick check
            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index] > a->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > a->GPU_mem_line
            //          && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line)
            //         {
            //             area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j] > a->GPU_mem_line)
            //                 {
            //                     area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
            //                 }
            //             }
            //         }
            //     }
            //     else
            //     {
            //         int offload_mid_index;
            //         int prefetch_mid_index;
            //         int offload_out = -1;
            //         int prefetch_out;
            //         // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
            //         // {
            //         //     offload_mid_index = gpu2ssd_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[0]);
            //         //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(a->the_tensor->size_in_byte, a->kernelLevel_interval[1]);
            //         // }
            //         // else {
            //         offload_mid_index = gpu2ssd_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], true, a->kernelLevel_interval[1], offload_out);
            //         prefetch_mid_index = ssd2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], true, a->kernelLevel_interval[0], prefetch_out);
            //         // }
            //         if (((offload_out >= a->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
            //         {
            //             offload_mid_index = gpu2pcie_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[0], true, a->kernelLevel_interval[1], offload_out);
            //             prefetch_mid_index = pcie2gpu_BWgiveIndx(a->the_tensor->size_in_byte, a->kernelLevel_interval[1], true, a->kernelLevel_interval[0], prefetch_out);
            //         }


            //         //Optimization: quick check
            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > a->GPU_mem_line
            //             && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line)
            //         {
            //             area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[((prefetch_mid_index-1) % kernel_num) + 1] - kernel_time_table[offload_mid_index % kernel_num]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j % kernel_num] > a->GPU_mem_line)
            //                 {
            //                     area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
            //                 }
            //             }
            //         }
                    

            //         // for (int j = a->kernelLevel_interval[0]; j < a->kernelLevel_interval[1] + kernel_list.size(); j++)
            //         // {
            //         //     if (GPU_resident_memory_estimation[j%kernel_list.size()] > a->GPU_mem_line)
            //         //     {
            //         //         area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[(j+1)%(kernel_list.size()+1)] - kernel_time_table[j%(kernel_list.size()+1)]);
            //         //     }
            //         // }
            //     }
            // }
            
            // if (b->is_offloaded || b->the_tensor->size_in_byte < 2000000)
            // {
            //     area_can_reduce_b = 0;
            //     b->is_offloaded = true;
            // }
            // else if (a->is_offloaded)
            // {
            //     area_can_reduce_b = 100000;
            // }
            // else
            // {
            //     if (!(b->is_looped))
            //     {
            //         int offload_mid_index;
            //         int prefetch_mid_index;
            //         int offload_out = -1;
            //         int prefetch_out;
            //         // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
            //         // {
            //         //     offload_mid_index = gpu2ssd_BWgiveIndx_half(b->the_tensor->size_in_byte, b->kernelLevel_interval[0]);
            //         //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(b->the_tensor->size_in_byte, b->kernelLevel_interval[1]);
            //         // }
            //         // else {
            //         offload_mid_index = gpu2ssd_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[0], false, b->kernelLevel_interval[1], offload_out);
            //         prefetch_mid_index = ssd2gpu_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[1], false, b->kernelLevel_interval[0], prefetch_out);
            //         // }
            //         if (((offload_out >= b->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - b->the_tensor->size_in_byte, b->kernelLevel_interval[0], b->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
            //         {
            //             offload_mid_index = gpu2pcie_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[0], false, b->kernelLevel_interval[1], offload_out);
            //             prefetch_mid_index = pcie2gpu_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[1], false, b->kernelLevel_interval[0], prefetch_out);
            //         }


            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index] > b->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > b->GPU_mem_line
            //             && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > b->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > b->GPU_mem_line)
            //         {
            //             area_can_reduce_b = b->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j] > b->GPU_mem_line)
            //                 {
            //                     area_can_reduce_b += b->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
            //                 }
            //             }
            //         }
                    
            //     }
            //     else
            //     {
            //         int offload_mid_index;
            //         int prefetch_mid_index;
            //         int offload_out = -1;
            //         int prefetch_out;
            //         // if (migration_policy_str!="G10GDSSSD" && migration_policy_str!="G10GDSFULL")
            //         // {
            //         //     offload_mid_index = gpu2ssd_BWgiveIndx_half(b->the_tensor->size_in_byte, b->kernelLevel_interval[0]);
            //         //     prefetch_mid_index = ssd2gpu_BWgiveIndx_half(b->the_tensor->size_in_byte, b->kernelLevel_interval[1]);
            //         // }
            //         // else {
            //         offload_mid_index = gpu2ssd_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[0], true, b->kernelLevel_interval[1], offload_out);
            //         prefetch_mid_index = ssd2gpu_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[1], true, b->kernelLevel_interval[0], prefetch_out);
            //         // }
            //         if (((offload_out >= b->kernelLevel_interval[0] + 10) || offload_out == -1) && check_CPU_OK_interval(CPU_line - b->the_tensor->size_in_byte, b->kernelLevel_interval[0], b->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
            //         {
            //             offload_mid_index = gpu2pcie_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[0], true, b->kernelLevel_interval[1], offload_out);
            //             prefetch_mid_index = pcie2gpu_BWgiveIndx(b->the_tensor->size_in_byte, b->kernelLevel_interval[1], true, b->kernelLevel_interval[0], prefetch_out);
            //         }


            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index % kernel_num] > b->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > b->GPU_mem_line
            //             && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > b->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > b->GPU_mem_line)
            //         {
            //             area_can_reduce_b = b->the_tensor->size_in_byte * (kernel_time_table[((prefetch_mid_index-1) % kernel_num) + 1] - kernel_time_table[offload_mid_index % kernel_num]);
            //         }
            //         else
            //         {
                        
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j % kernel_num] > b->GPU_mem_line)
            //                 {
            //                     area_can_reduce_b += b->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
            //                 }
            //             }
            //         }

            //         // for (int j = b->kernelLevel_interval[0]; j < b->kernelLevel_interval[1] + kernel_list.size(); j++)
            //         // {
            //         //     if (GPU_resident_memory_estimation[j%kernel_list.size()] > b->GPU_mem_line)
            //         //     {
            //         //         area_can_reduce_b += b->the_tensor->size_in_byte * (kernel_time_table[(j+1)%(kernel_list.size()+1)] - kernel_time_table[j%(kernel_list.size()+1)]);
            //         //     }
            //         // }
            //     }
            // }
            

            if (area_can_reduce_a != 0 && area_can_reduce_b != 0 && ((a->the_tensor->size_in_byte) - (b->the_tensor->size_in_byte) < 200000000) && ((a->the_tensor->size_in_byte) - (b->the_tensor->size_in_byte) > -200000000) && (a->the_tensor->size_in_byte) > 2000000 && (b->the_tensor->size_in_byte) > 2000000)
            {
                return (area_can_reduce_a / (a->the_tensor->size_in_byte)) > (area_can_reduce_b / (b->the_tensor->size_in_byte));
            }
            else 
            {
                return (area_can_reduce_a > area_can_reduce_b);
            }
        });

        bool need_to_break = false;

        for (int kk = 0; kk < Sample; kk++)
        {
            /* code */
        
        
            // The interval list is already sorted
            Hidding_Interval* curr_interval = interval_list[kk];
            // curr_interval->print();
            if (curr_interval->is_offloaded)
            {
                cold_period_iter = -1;
                need_to_break = true;
                break;
            }
            
            if (check_GPU_OK_interval(target_mem_line, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]))
            {
                curr_interval->is_offloaded = true;
                continue;
            }

            if (curr_interval->the_tensor->size_in_byte < 2000000)
            {
                curr_interval->is_offloaded = true;
                continue;
            }

            int cha;
            if (!curr_interval->is_looped)
            {
                cha = curr_interval->kernelLevel_interval[1] - curr_interval->kernelLevel_interval[0];
            }
            else
            {
                cha = curr_interval->kernelLevel_interval[1] + kernel_num - curr_interval->kernelLevel_interval[0];
            }
            if (cha==1)
            {
                curr_interval->is_offloaded = true;
                continue;
            }



            //Check for the pinned memory 
            if (GPU_resident_memory_estimation_pinned[(curr_interval->kernelLevel_interval[0])%kernel_num] + curr_interval->the_tensor->size_in_byte > target_mem_line)
            {
                curr_interval->is_offloaded = true;
                continue;  
            }
            if (GPU_resident_memory_estimation_pinned[(curr_interval->kernelLevel_interval[1]+kernel_num-1)%kernel_num] + curr_interval->the_tensor->size_in_byte > target_mem_line)
            {
                curr_interval->is_offloaded = true;
                continue;  
            }

            double ssd_safe_time = 2*(SSD_latency_us + system_latency_us + curr_interval->the_tensor->size_in_byte / (double)(SSD_PCIe_bandwidth_GBps*1024*1024*1024/1000000));
            double ssd_movement_estimated_time = ssd_safe_time / 2;
            double delta_time = delta_parameter * ssd_movement_estimated_time; //TODO: Very Important, we need to Prune this
            ssd_safe_time += delta_time;
            double ssd_prefetch_estimated_time = ssd_movement_estimated_time + delta_time;

            double cpu_safe_time = 2*(system_latency_us + curr_interval->the_tensor->size_in_byte / (double)(CPU_PCIe_bandwidth_GBps*1024*1024*1024/1000000));
            double cpu_movement_estimated_time = cpu_safe_time / 2;
            double delta_cpu_time = delta_parameter * cpu_movement_estimated_time; //TODO: Very Important, we need to Prune this
            cpu_safe_time += delta_cpu_time;
            double cpu_prefetch_estimated_time = cpu_movement_estimated_time + delta_cpu_time;

            // if (curr_interval->time_estimated > ssd_safe_time)
            // {
            if (!curr_interval->is_looped)  //Not looped
            {
                
                //Find the  ideal  finished(clear) index
                // double eviction_finish_time = kernel_time_table[curr_interval->kernelLevel_interval[0]] + ssd_movement_estimated_time;
                // int eviction_clear_index = -1;
                // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1]; j++)
                // {
                //     if (kernel_time_table[j] <= eviction_finish_time && kernel_time_table[j+1] > eviction_finish_time)
                //     {
                //         eviction_clear_index = j;
                //         break;
                //     }
                // }
                // Assert(eviction_clear_index >= 0);


                //NEW: Use PCIe estimation to get the finishing index
                int pcie_eviction_clear_index = -1;
                // pcie_eviction_clear_index = gpu2ssd_BWcheck(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], eviction_clear_index, false, curr_interval->kernelLevel_interval[1]);


                //Second calculate the prefetch ideal index
                // double prefetch_start_time_precise = kernel_time_table[curr_interval->kernelLevel_interval[1]] - ssd_prefetch_estimated_time;
                // int prefetch_start_index = -1;
                // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1]; j++)
                // {
                //     if (kernel_time_table[j] <= prefetch_start_time_precise && kernel_time_table[j+1] > prefetch_start_time_precise)
                //     {
                //         prefetch_start_index = j;
                //         break;
                //     }
                // }
                // Assert(prefetch_start_index>=0);

                int pcie_prefetch_index = -1;
                bool abort = false;

                if (0)
                // if (pcie_eviction_clear_index==-1 && check_CPU_OK(CPU_line - curr_interval->the_tensor->size_in_byte)) //SSD is not OK, CPU is OK
                {
                    //Calculate cpu-prefetch-index
                    // pcie_prefetch_index = pcie2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte*1.0, curr_interval->kernelLevel_interval[1]);
                    // Assert(pcie_prefetch_index >=0);
                    // // Get cpu eviction finish index
                    // pcie_eviction_clear_index = gpu2pcie_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0]);
                    // Assert(pcie_eviction_clear_index >=0);

                    // if(pcie_prefetch_index > pcie_eviction_clear_index){
                    //     //First schedule the pre-eviction
                    //     DataMovementHint pre_evict(PageLocation::NOT_KNOWN, PageLocation::IN_CPU, curr_interval->kernelLevel_interval[0], curr_interval->the_tensor);
                    //     pre_evict.barrier_end_time = pcie_eviction_clear_index;
                    //     movement_hints.push_back(pre_evict);
                    //     //Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;
                    //     curr_interval->the_tensor->is_choosed_to_evict = true;
                    //     curr_interval->is_really_offloaded = true;

                    //     // DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, pcie_prefetch_index, curr_interval->the_tensor);
                    //     // movement_hints.push_back(pre_fetch);
                    //     curr_interval->original_prefetch_index = pcie_prefetch_index;
                    //     curr_interval->evict_finish_index = pcie_eviction_clear_index;
                    //     offloeded_local_intervals.push_back(curr_interval);

                    //     pcie2gpu_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index);
                    //     gpu2pcie_BWsim(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0]);
                    //     CPU_add_update_interval(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]);

                    // }
                    // else{
                    //     curr_interval->is_offloaded  = true;
                    //     continue;
                    // }
                }
                else // TODO: start from here
                {
                    //Calculate ssd-prefetch-index
                    bool offload_to_cpu = false;
                    int offload_out_index = -1;
                    int prefetch_out_index = -1;
                    pcie_prefetch_index = ssd2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[1], curr_interval->is_looped, curr_interval->kernelLevel_interval[0], prefetch_out_index);
                    Assert(pcie_prefetch_index >=0);
                    // Get cpu eviction finish index
                    pcie_eviction_clear_index = gpu2ssd_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->is_looped, curr_interval->kernelLevel_interval[1], offload_out_index);
                    Assert(pcie_eviction_clear_index >=0);

                    if (((offload_out_index >= curr_interval->kernelLevel_interval[0] + 10) || offload_out_index == -1) && check_CPU_OK_interval(CPU_line - curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
                    {
                        pcie_eviction_clear_index = gpu2pcie_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->is_looped, curr_interval->kernelLevel_interval[1], offload_out_index);
                        pcie_prefetch_index = pcie2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[1], curr_interval->is_looped, curr_interval->kernelLevel_interval[0], prefetch_out_index);
                        offload_to_cpu = true;
                    }

                    
                    for (int j = curr_interval->kernelLevel_interval[0]; j < pcie_eviction_clear_index; j++)
                    {
                        // GPU_resident_memory_estimation_pinned[j] += curr_interval->the_tensor->size_in_byte;
                        if ((GPU_resident_memory_estimation_pinned[j] + curr_interval->the_tensor->size_in_byte )> target_mem_line*0.90)
                        {
                            abort = true;
                            break;
                        }
                    }
                    for (int j = pcie_prefetch_index; j < curr_interval->kernelLevel_interval[1]; j++)
                    {
                        // GPU_resident_memory_estimation_pinned[j] += curr_interval->the_tensor->size_in_byte;
                        if ((GPU_resident_memory_estimation_pinned[j] + curr_interval->the_tensor->size_in_byte) > target_mem_line*0.90)
                        {
                            abort = true;
                            break;
                        }
                    }
                    if (!abort)
                    {
                        for (int j = curr_interval->kernelLevel_interval[0]; j < pcie_eviction_clear_index; j++)
                        {
                            GPU_resident_memory_estimation_pinned[j] += curr_interval->the_tensor->size_in_byte;
                        }
                        for (int j = pcie_prefetch_index; j < curr_interval->kernelLevel_interval[1]; j++)
                        {
                            GPU_resident_memory_estimation_pinned[j] += curr_interval->the_tensor->size_in_byte;
                        }
                    }
                    
                    if(pcie_prefetch_index > pcie_eviction_clear_index && !abort){
                        //First schedule the pre-eviction
                        DataMovementHint pre_evict(PageLocation::NOT_KNOWN, offload_to_cpu ? PageLocation::IN_CPU : PageLocation::IN_SSD, offload_out_index, curr_interval->the_tensor);
                        pre_evict.barrier_end_time = pcie_eviction_clear_index % kernel_num;
                        pre_evict.p_order = pcie_eviction_clear_index;
                        pre_evict.direction = offload_to_cpu ? MigrationDirection::G2C : MigrationDirection::G2S;
                        global_p_order_pos++;
                        movement_hints.push_back(pre_evict);
                        curr_interval->the_tensor->is_choosed_to_evict = true;
                        curr_interval->offload_to_cpu = offload_to_cpu;
                        curr_interval->is_really_offloaded = true;
                        curr_interval->is_offloaded = true;

                        // DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, pcie_prefetch_index, curr_interval->the_tensor);
                        // movement_hints.push_back(pre_fetch);
                        curr_interval->original_prefetch_index = pcie_prefetch_index % kernel_num;
                        curr_interval->original_prefetch_finish_index = prefetch_out_index % kernel_num;
                        curr_interval->evict_finish_index = pcie_eviction_clear_index % kernel_num;
                        offloeded_local_intervals.push_back(curr_interval);
        
                        // std::cout<<"Prefetch tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->original_prefetch_index<<" to "<<curr_interval->original_prefetch_finish_index<<std::endl;

                        Assert(offload_out_index >= 0);
                        Assert(prefetch_out_index >= 0);

                        if (offload_to_cpu)
                        {
                            CPU_add_update_interval(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]);
                            gpu2pcie_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index);
                            pcie2gpu_BWsim(curr_interval->the_tensor->size_in_byte, offload_out_index);
                        }
                        else
                        {
                            ssd2gpu_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index);
                            gpu2ssd_BWsim(curr_interval->the_tensor->size_in_byte, offload_out_index);
                        }

                    }
                    else{
                        curr_interval->is_offloaded = true;
                        continue;
                    }
                }
                
                //minus mem
                Assert(pcie_eviction_clear_index>=0);
                
                if (!abort)
                {
                    for (int j = pcie_eviction_clear_index + 1; j < pcie_prefetch_index; j++)
                    {
                        GPU_resident_memory_estimation[j] -= curr_interval->the_tensor->size_in_byte;
                    }
                }
                
                curr_interval->is_offloaded = true;
            }
            else
            {           
                // //Find the finished(clear) index
                // double eviction_finish_time = kernel_time_table[curr_interval->kernelLevel_interval[0]] + ssd_movement_estimated_time;
                // int eviction_clear_index = -1;
                // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1] + kernel_num; j++)  // j is the extended table index
                // {
                //     if (kernel_time_table_extended[j] <= eviction_finish_time && kernel_time_table_extended[j+1] > eviction_finish_time)
                //     {
                //         eviction_clear_index = j;
                //         break;
                //     }
                // }
                // Assert(eviction_clear_index >= 0);


                // //Second schedule the prefetch
                // double prefetch_start_time_precise = kernel_time_table_extended[curr_interval->kernelLevel_interval[1] + kernel_num] - ssd_prefetch_estimated_time;
                // int prefetch_start_index = -1;
                // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1] + kernel_num; j++)
                // {
                //     if (kernel_time_table_extended[j] <= prefetch_start_time_precise && kernel_time_table_extended[j+1] > prefetch_start_time_precise)
                //     {
                //         prefetch_start_index = j;
                //         break;
                //     }
                // }
                // Assert(prefetch_start_index>=0);

                // if (prefetch_start_index!=curr_interval->kernelLevel_interval[0])
                // {
                //     //First schedule the pre-eviction
                //     DataMovementHint pre_evict(PageLocation::NOT_KNOWN, PageLocation::IN_SSD, curr_interval->kernelLevel_interval[0], curr_interval->the_tensor);
                //     movement_hints.push_back(pre_evict);
                //     curr_interval->the_tensor->is_choosed_to_evict = true;
                //     curr_interval->is_really_offloaded = true;

                //     DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, prefetch_start_index % kernel_num, curr_interval->the_tensor);
                //     movement_hints.push_back(pre_fetch);
                // }

                //Calculate ssd-prefetch-index
                int pcie_eviction_clear_index = -1;
                bool offload_to_cpu = false;
                int pcie_prefetch_index = -1;
                int offload_out_index = -1;
                int prefetch_out_index = -1;
                bool abort = false;

                pcie_prefetch_index = ssd2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[1], curr_interval->is_looped, curr_interval->kernelLevel_interval[0], prefetch_out_index);
                Assert(pcie_prefetch_index >=0);
                // Get cpu eviction finish index
                pcie_eviction_clear_index = gpu2ssd_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->is_looped, curr_interval->kernelLevel_interval[1], offload_out_index);
                Assert(pcie_eviction_clear_index >=0);

                if (((offload_out_index >= curr_interval->kernelLevel_interval[0] + 10) || offload_out_index == -1) && check_CPU_OK_interval(CPU_line - curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1])) // SSD bandwidth is somehow busy
                {
                    pcie_eviction_clear_index = gpu2pcie_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->is_looped, curr_interval->kernelLevel_interval[1], offload_out_index);
                    pcie_prefetch_index = pcie2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[1], curr_interval->is_looped, curr_interval->kernelLevel_interval[0], prefetch_out_index);
                    offload_to_cpu = true;
                }

                for (int j = curr_interval->kernelLevel_interval[0]; j < pcie_eviction_clear_index; j++)
                {
                    // GPU_resident_memory_estimation_pinned[(j%kernel_num)] += curr_interval->the_tensor->size_in_byte;
                    if ((GPU_resident_memory_estimation_pinned[(j%kernel_num)] + curr_interval->the_tensor->size_in_byte) > target_mem_line*0.90)
                    {
                        abort = true;
                        break;
                    }
                    
                }
                for (int j = pcie_prefetch_index; j < curr_interval->kernelLevel_interval[1] +kernel_num; j++)
                {
                    // GPU_resident_memory_estimation_pinned[(j%kernel_num)] += curr_interval->the_tensor->size_in_byte;
                    if ((GPU_resident_memory_estimation_pinned[(j%kernel_num)] + curr_interval->the_tensor->size_in_byte) > target_mem_line*0.90)
                    {
                        abort = true;
                        break;
                    }
                    
                }
                
                if (!abort)
                {
                    for (int j = curr_interval->kernelLevel_interval[0]; j < pcie_eviction_clear_index; j++)
                    {
                        GPU_resident_memory_estimation_pinned[(j%kernel_num)] += curr_interval->the_tensor->size_in_byte;
                    }
                    for (int j = pcie_prefetch_index; j < curr_interval->kernelLevel_interval[1] +kernel_num; j++)
                    {
                        GPU_resident_memory_estimation_pinned[(j%kernel_num)] += curr_interval->the_tensor->size_in_byte;
                    }
                }
                
                

                if(pcie_prefetch_index > pcie_eviction_clear_index && !abort){
                    //First schedule the pre-eviction
                    DataMovementHint pre_evict(PageLocation::NOT_KNOWN, offload_to_cpu ? PageLocation::IN_CPU : PageLocation::IN_SSD, offload_out_index % kernel_num, curr_interval->the_tensor);
                    pre_evict.barrier_end_time = pcie_eviction_clear_index % kernel_num;
                    pre_evict.p_order = pcie_eviction_clear_index;
                    pre_evict.direction = offload_to_cpu ? MigrationDirection::G2C : MigrationDirection::G2S;
                    global_p_order_pos++;
                    movement_hints.push_back(pre_evict);
                    //Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;
                    curr_interval->the_tensor->is_choosed_to_evict = true;
                    curr_interval->offload_to_cpu = offload_to_cpu;
                    curr_interval->is_really_offloaded = true;

                    // DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, pcie_prefetch_index, curr_interval->the_tensor);
                    // movement_hints.push_back(pre_fetch);
                    curr_interval->original_prefetch_index = pcie_prefetch_index % kernel_num;
                    curr_interval->original_prefetch_finish_index = prefetch_out_index % kernel_num;
                    curr_interval->evict_finish_index = pcie_eviction_clear_index % kernel_num;
                    offloeded_local_intervals.push_back(curr_interval);

                    // std::cout<<"Prefetch tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->original_prefetch_index<<" to "<<curr_interval->original_prefetch_finish_index<<std::endl;

                    Assert(offload_out_index >= 0);
                    Assert(prefetch_out_index >= 0);

                    if (offload_to_cpu)
                    {
                        CPU_add_update_interval(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]);
                        gpu2pcie_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index % kernel_num);
                        pcie2gpu_BWsim(curr_interval->the_tensor->size_in_byte, offload_out_index % kernel_num);
                    }
                    else
                    {
                        ssd2gpu_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index % kernel_num);
                        gpu2ssd_BWsim(curr_interval->the_tensor->size_in_byte, offload_out_index % kernel_num);
                    }
                    
                }
                else{
                    curr_interval->is_offloaded = true;
                    continue;
                }

                //minus mem

                if (!abort)
                {
                    for (int j = pcie_eviction_clear_index + 1; j < pcie_prefetch_index; j++)
                    {
                        GPU_resident_memory_estimation[(j%kernel_num)] -= curr_interval->the_tensor->size_in_byte;
                    }
                }
                

                curr_interval->is_offloaded = true;
            }
                
                
            // }
            // else if (curr_interval->time_estimated > cpu_safe_time)
            // {
            //     /* code */
            //     if (!check_CPU_OK(CPU_line - curr_interval->the_tensor->size_in_byte))    //If already full, end this loop iteration
            //     {
            //         curr_interval->is_offloaded = true;
            //         continue;
            //     }


            //     if (!curr_interval->is_looped)  //Not looped
            //     {
                    
            //         //Find the finished(clear) index
            //         // double eviction_finish_time = kernel_time_table[curr_interval->kernelLevel_interval[0]] + cpu_movement_estimated_time;
            //         // int eviction_clear_index = -1;
            //         // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1]; j++)
            //         // {
            //         //     if (kernel_time_table[j] <= eviction_finish_time && kernel_time_table[j+1] > eviction_finish_time)
            //         //     {
            //         //         eviction_clear_index = j;
            //         //         break;
            //         //     }
            //         // }
            //         // Assert(eviction_clear_index >= 0);

            //         //NEW: Use PCIe estimation to get the finishing index
            //         int pcie_eviction_clear_index = -1;
            //         pcie_eviction_clear_index = gpu2pcie_BWgiveIndx(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0]);
            //         Assert(pcie_eviction_clear_index>=0);


            //         //Second schedule the prefetch
            //         // double prefetch_start_time_precise = kernel_time_table[curr_interval->kernelLevel_interval[1]] - cpu_prefetch_estimated_time;
            //         // int prefetch_start_index = -1;
            //         // for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1]; j++)
            //         // {
            //         //     if (kernel_time_table[j] <= prefetch_start_time_precise && kernel_time_table[j+1] > prefetch_start_time_precise)
            //         //     {
            //         //         prefetch_start_index = j;
            //         //         break;
            //         //     }
            //         // }
            //         // Assert(prefetch_start_index>=0);

            //         int pcie_prefetch_index = -1;
            //         pcie_prefetch_index = pcie2gpu_BWgiveIndx(curr_interval->the_tensor->size_in_byte*1.0, curr_interval->kernelLevel_interval[1]);
            //         Assert(pcie_prefetch_index >=0);


            //         if (pcie_prefetch_index > pcie_eviction_clear_index)
            //         {
            //             //First schedule the pre-eviction
            //             DataMovementHint pre_evict(PageLocation::NOT_KNOWN, PageLocation::IN_CPU, curr_interval->kernelLevel_interval[0], curr_interval->the_tensor);
            //             pre_evict.barrier_end_time = pcie_eviction_clear_index;
            //             movement_hints.push_back(pre_evict);
            //             //Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;
            //             curr_interval->the_tensor->is_choosed_to_evict = true;
            //             curr_interval->is_really_offloaded = true;

            //             // DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, pcie_prefetch_index, curr_interval->the_tensor);
            //             // movement_hints.push_back(pre_fetch);
            //             curr_interval->original_prefetch_index = pcie_prefetch_index;
            //             curr_interval->evict_finish_index = pcie_eviction_clear_index;
            //             offloeded_local_intervals.push_back(curr_interval);

            //             pcie2gpu_BWsim(curr_interval->the_tensor->size_in_byte, pcie_prefetch_index);
            //             gpu2pcie_BWsim(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0]);
            //         }

            //         //minus mem
            //         Assert(pcie_eviction_clear_index>=0);
            //         for (int j = pcie_eviction_clear_index + 1; j < pcie_prefetch_index; j++)
            //         {
            //             GPU_resident_memory_estimation[j] -= curr_interval->the_tensor->size_in_byte;
            //         }
            //         curr_interval->is_offloaded = true;
            //     }
            //     else
            //     {

                    
            //         //Find the finished(clear) index
            //         double eviction_finish_time = kernel_time_table[curr_interval->kernelLevel_interval[0]] + cpu_movement_estimated_time;
            //         int eviction_clear_index = -1;
            //         for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1] + kernel_num; j++)  // j is the extended table index
            //         {
            //             if (kernel_time_table_extended[j] <= eviction_finish_time && kernel_time_table_extended[j+1] > eviction_finish_time)
            //             {
            //                 eviction_clear_index = j;
            //                 break;
            //             }
            //         }
            //         Assert(eviction_clear_index >= 0);


            //         //Second schedule the prefetch
            //         double prefetch_start_time_precise = kernel_time_table_extended[curr_interval->kernelLevel_interval[1] + kernel_num] - cpu_prefetch_estimated_time;
            //         int prefetch_start_index = -1;
            //         for (int j = curr_interval->kernelLevel_interval[0]; j < curr_interval->kernelLevel_interval[1] + kernel_num; j++)
            //         {
            //             if (kernel_time_table_extended[j] <= prefetch_start_time_precise && kernel_time_table_extended[j+1] > prefetch_start_time_precise)
            //             {
            //                 prefetch_start_index = j;
            //                 break;
            //             }
            //         }
            //         Assert(prefetch_start_index>=0);

            //         if (prefetch_start_index!=curr_interval->kernelLevel_interval[0])
            //         {
            //             //First schedule the pre-eviction
            //             DataMovementHint pre_evict(PageLocation::NOT_KNOWN, PageLocation::IN_CPU, curr_interval->kernelLevel_interval[0], curr_interval->the_tensor);
            //             pre_evict.barrier_end_time = eviction_clear_index;
            //             movement_hints.push_back(pre_evict);
            //             //Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;
            //             curr_interval->the_tensor->is_choosed_to_evict = true;
            //             curr_interval->is_really_offloaded = true;

            //             DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, prefetch_start_index % kernel_num, curr_interval->the_tensor);
            //             pre_fetch.barrier_end_time = curr_interval->is_looped ? (((curr_interval->kernelLevel_interval[1] + kernel_num -1)%kernel_num) > (prefetch_start_index % kernel_num) ? ((curr_interval->kernelLevel_interval[1] + kernel_num -1)%kernel_num) : (curr_interval->kernelLevel_interval[1] + kernel_num -1) ) : (curr_interval->kernelLevel_interval[1] -1);
            //             movement_hints.push_back(pre_fetch);
            //             //Pre-fetch tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<prefetch_start_index % kernel_num<<std::endl;
            //         }
                    

            //         //minus mem
            //         for (int j = eviction_clear_index + 1; j < prefetch_start_index; j++)
            //         {
            //             GPU_resident_memory_estimation[(j%kernel_num)] -= curr_interval->the_tensor->size_in_byte;
            //         }
            //         curr_interval->is_offloaded = true;
            //     }

            //     CPU_add_update_interval(curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]);

            // }        
        }
        if (need_to_break)
        {
            std::cout<<"1_Need_to_break"<<std::endl;
            for (int i = 0; i < interval_list.size(); i++)
            {
                std::cout<<"Interval "<<i<<std::endl;
                std::cout<<"is_really_offloaded = "<<interval_list[i]->is_really_offloaded<<std::endl;        
                interval_list[i]->print();
            }
            break;
        }

    }



    std::cerr<<"cold_iter = "<<cold_period_iter<<std::endl;

    std::cerr << "After PCIE-aware Offloading" << std::endl;
    print_GPU_mem_estimation("pcie_aware_offloaded");



    // std::cerr<<"BW Estimation:"<<std::endl;
    print_BW_estimations();


    // std::cerr<<"Now scheduling local prefetch!"<<std::endl;
    std::sort(offloeded_local_intervals.begin(), offloeded_local_intervals.end(), [](Hidding_Interval* a, Hidding_Interval* b){
        return a->original_prefetch_index < b->original_prefetch_index;
    });
    
    int previous_iteration_final_index = -1;

    for (int i = 0; i < offloeded_local_intervals.size(); i++)
    {
        Hidding_Interval* current_interv = offloeded_local_intervals[i];
        // std::cerr<<current_interv->original_prefetch_index<<std::endl;

        bool previous_interv_is_looped = false;
        if (previous_iteration_final_index!= -1 && previous_iteration_final_index >= current_interv->original_prefetch_index)
        {
            //Looped
            previous_interv_is_looped = true;
        }
        

        int iindx = current_interv->original_prefetch_index;

        // std::cout<<"Adjusting tensor "<<current_interv->the_tensor->tensor_id<<" at kernel ID "<<current_interv->original_prefetch_index<<" to "<<current_interv->original_prefetch_finish_index<<std::endl;
        // std::cout<<"Previous iteration final index "<<previous_iteration_final_index<<std::endl;
        // std::cout<<"Evict finish index "<<current_interv->evict_finish_index<<std::endl;
        
        if (current_interv->original_prefetch_index <= current_interv->evict_finish_index)
        {
            //Looped
            iindx += kernel_num;
            current_interv->original_prefetch_index += kernel_num;
            while ((iindx > current_interv->evict_finish_index + 1) && (iindx > previous_iteration_final_index))
            {
                if (target_mem_line > GPU_resident_memory_estimation[(iindx - 1) % kernel_num] + current_interv->the_tensor->size_in_byte)
                {
                    iindx--;
                }
                else
                {
                    break;
                }
            }
        }
        else
        {
            if (previous_interv_is_looped)
            {
                while ((iindx > current_interv->evict_finish_index + 1))
                {
                    if (target_mem_line > GPU_resident_memory_estimation[iindx - 1] + current_interv->the_tensor->size_in_byte)
                    {
                        iindx--;
                    }
                    else
                    {
                        break;
                    }
                }
            }
            else
            {
                while ((iindx > current_interv->evict_finish_index + 1) && (iindx > previous_iteration_final_index))
                {
                    if (target_mem_line > GPU_resident_memory_estimation[iindx - 1] + current_interv->the_tensor->size_in_byte)
                    {
                        iindx--;
                    }
                    else
                    {
                        break;
                    }
                }
            }    
        }
        
        // std::cout<<"Adjusted prefetch index "<<iindx<<std::endl;
        
        DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, iindx%kernel_num, current_interv->the_tensor);
        previous_iteration_final_index = iindx;
        pre_fetch.direction = current_interv->offload_to_cpu ? MigrationDirection::C2G : MigrationDirection::S2G;
        // pre_fetch.barrier_end_time = current_interv->is_looped ? (((current_interv->kernelLevel_interval[1] + kernel_num -1)%kernel_num) > (iindx%kernel_num) ? ((current_interv->kernelLevel_interval[1] + kernel_num -1)%kernel_num) : (current_interv->kernelLevel_interval[1] + kernel_num -1) ) : (current_interv->kernelLevel_interval[1] -1);
        pre_fetch.barrier_end_time = (current_interv->original_prefetch_finish_index) % kernel_num;
        pre_fetch.p_order = current_interv->original_prefetch_finish_index;
        global_p_order_pos++;
        movement_hints.push_back(pre_fetch);
        //Pre-fetch tensor "<<current_interv->the_tensor->tensor_id<<" at kernel ID "<<iindx%kernel_num<<std::endl;

        //plus mem
        for (int j = iindx; j < current_interv->original_prefetch_index; j++)
        {
            GPU_resident_memory_estimation[j%kernel_num] += current_interv->the_tensor->size_in_byte;
        }
    }


    std::cerr << "---------After Second-time Modification" << std::endl;
    print_GPU_mem_estimation("After_Prefetch_adjusted");


    // Avoid insert emergent migrations into ongoing migration
    kernel_no_ongoing_mirgation_list_S2G.resize(kernel_num);
    kernel_no_ongoing_mirgation_list_G2S.resize(kernel_num);
    kernel_no_ongoing_mirgation_list_C2G.resize(kernel_num);
    kernel_no_ongoing_mirgation_list_G2C.resize(kernel_num);

    for (int i = 0; i < kernel_num; i++)
    {
        kernel_no_ongoing_mirgation_list_S2G[i] = false;
        kernel_no_ongoing_mirgation_list_G2S[i] = false;
        kernel_no_ongoing_mirgation_list_C2G[i] = false;
        kernel_no_ongoing_mirgation_list_G2C[i] = false;
    }

    for (int i = 0; i < movement_hints.size(); i++)
    {
        if (movement_hints[i].direction == MigrationDirection::S2G)
        {
            kernel_no_ongoing_mirgation_list_S2G[(movement_hints[i].barrier_end_time-1)%kernel_num] = true;
        }
        else if (movement_hints[i].direction == MigrationDirection::G2S)
        {
            kernel_no_ongoing_mirgation_list_G2S[movement_hints[i].issued_time] = true;
        }
        else if (movement_hints[i].direction == MigrationDirection::C2G)
        {
            kernel_no_ongoing_mirgation_list_C2G[(movement_hints[i].barrier_end_time-1)%kernel_num] = true;
        }
        else if (movement_hints[i].direction == MigrationDirection::G2C)
        {
            kernel_no_ongoing_mirgation_list_G2C[movement_hints[i].issued_time] = true;
        }
    }

    for (int i = 0; i < kernel_num; i++)
    {
        if (ssd2gpu_BW_estimation[i].capacity < 1.0)
        {
            kernel_no_ongoing_mirgation_list_S2G[i] = true;
        }
        if (gpu2ssd_BW_estimation[i].capacity < 1.0)
        {
            kernel_no_ongoing_mirgation_list_G2S[i] = true;
        }
        if (!gpu2pcie_BW_estimation[i].full)
        {
            kernel_no_ongoing_mirgation_list_G2C[i] = true;
        }
        if (!pcie2gpu_BW_estimation[i].full)
        {
            kernel_no_ongoing_mirgation_list_C2G[i] = true;
        }
    }
    
    
    
    //Fill the looped extend kernel time table      0 - 2 * kernel_num
    
    kernel_time_table_extended_sort.resize(kernel_num);
    for (int j = 0; j < kernel_num; j++)
    {
        kernel_time_table_extended_sort[j] = kernel_time_table[j];
    }
    double last_time_s = kernel_time_table[kernel_num];
    kernel_time_table_extended_sort.push_back(last_time_s);
    for (int j = 0; j < kernel_num; j++)
    {
        last_time_s += (double)kernel_list[j].execution_cycles / (double)(GPU_frequency_GHz*1000);
        kernel_time_table_extended_sort.push_back(last_time_s);
    }



    // Cold periods second pass, for smart eviction policy
    for (int i = 0; i < interval_list.size(); i+=Sample*5)
    {
        
        if (check_GPU_OK(target_mem_line))    //If already OK, end this loop
        {
            break;
        }

        for (int j = 0; j < interval_list.size(); j++)
        {
            Hidding_Interval* a = interval_list[j];
            double area_can_reduce_a = 0;
            if (a->is_really_offloaded || a->the_tensor->size_in_byte < 1024*32)
            {
                area_can_reduce_a = 0;
            }
            else
            {
                if (!(a->is_looped))
                {
                    int offload_mid_index = -1;
                    int prefetch_mid_index = -1;
                    offload_mid_index = a->kernelLevel_interval[0];
                    prefetch_mid_index = a->kernelLevel_interval[1];

                    // bool cpu_ok = check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1]);
                    // bool cpu_doable = false;
                    // bool overall_doable = false;

                    // if (cpu_ok)
                    // {
                    //     bool find_offload = true;
                    //     // for (int j = a->kernelLevel_interval[0]; j < a->kernelLevel_interval[1]; j++)
                    //     // {
                    //     //     if (kernel_no_ongoing_mirgation_list_G2C[j])
                    //     //     {
                    //     //         offload_mid_index = j;
                    //     //         find_offload = true;
                    //     //         break;
                    //     //     }
                    //     // }
                    //     bool find_prefetch = false;
                    //     for (int j = a->kernelLevel_interval[1]; j > a->kernelLevel_interval[0]; j--)
                    //     {
                    //         if (kernel_no_ongoing_mirgation_list_C2G[j])
                    //         {
                    //             prefetch_mid_index = j;
                    //             find_prefetch = true;
                    //             break;
                    //         }
                    //     }
                    //     if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                    //     {
                    //         cpu_doable = true;
                    //         overall_doable = true;
                    //     }
                        
                    // }

                    // if(!cpu_doable){
                    //     bool find_offload = true;
                    //     // for (int j = a->kernelLevel_interval[0]; j < a->kernelLevel_interval[1]; j++)
                    //     // {
                    //     //     if (kernel_no_ongoing_mirgation_list_G2S[j])
                    //     //     {
                    //     //         offload_mid_index = j;
                    //     //         find_offload = true;
                    //     //         break;
                    //     //     }
                    //     // }
                    //     bool find_prefetch = false;
                    //     for (int j = a->kernelLevel_interval[1]; j > a->kernelLevel_interval[0]; j--)
                    //     {
                    //         if (kernel_no_ongoing_mirgation_list_S2G[j])
                    //         {
                    //             prefetch_mid_index = j;
                    //             find_prefetch = true;
                    //             break;
                    //         }
                    //     }
                    //     if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                    //     {
                    //         overall_doable = true;
                    //     }
                    // }

                    // if (!overall_doable)
                    // {
                    //     offload_mid_index = a->kernelLevel_interval[0];
                    //     prefetch_mid_index = a->kernelLevel_interval[1];
                    //     a->gar_for_step2_offloading = true;
                    // }
                    

                    //Optimization: quick check
                    if (offload_mid_index+1 < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index+1] > a->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > a->GPU_mem_line
                    && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line)
                    {
                        area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index+1]);
                    }
                    else
                    {
                        for (int j = offload_mid_index+1; j < prefetch_mid_index; j++)
                        {
                            if (GPU_resident_memory_estimation[j] > a->GPU_mem_line)
                            {
                                area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
                            }
                        }
                    }

                    // if (!overall_doable)
                    // {
                    //     area_can_reduce_a = area_can_reduce_a * 0.5;
                    // }

                }
                else
                {
                    int offload_mid_index = -1;
                    int prefetch_mid_index = -1;
                    offload_mid_index = a->kernelLevel_interval[0];
                    prefetch_mid_index = a->kernelLevel_interval[1] + kernel_num;


                    // bool cpu_ok = check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1]);
                    // bool cpu_doable = false;
                    // bool overall_doable = false;
                    // int needed_index = a->kernelLevel_interval[1] + kernel_num;

                    // if (cpu_ok)
                    // {
                    //     bool find_offload = true;
                    //     // for (int j = a->kernelLevel_interval[0]; j < needed_index; j++)
                    //     // {
                    //     //     if (kernel_no_ongoing_mirgation_list_G2C[j%kernel_num])
                    //     //     {
                    //     //         offload_mid_index = j;
                    //     //         find_offload = true;
                    //     //         break;
                    //     //     }
                    //     // }
                    //     bool find_prefetch = false;
                    //     for (int j = needed_index; j > a->kernelLevel_interval[0]; j--)
                    //     {
                    //         if (kernel_no_ongoing_mirgation_list_C2G[j%kernel_num])
                    //         {
                    //             prefetch_mid_index = j;
                    //             find_prefetch = true;
                    //             break;
                    //         }
                    //     }
                    //     if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                    //     {
                    //         cpu_doable = true;
                    //         overall_doable = true;
                    //     }
                        
                    // }

                    // if(!cpu_doable){
                    //     bool find_offload = true;
                    //     // for (int j = a->kernelLevel_interval[0]; j < needed_index; j++)
                    //     // {
                    //     //     if (kernel_no_ongoing_mirgation_list_G2S[j%kernel_num])
                    //     //     {
                    //     //         offload_mid_index = j;
                    //     //         find_offload = true;
                    //     //         break;
                    //     //     }
                    //     // }
                    //     bool find_prefetch = false;
                    //     for (int j = needed_index; j > a->kernelLevel_interval[0]; j--)
                    //     {
                    //         if (kernel_no_ongoing_mirgation_list_S2G[j%kernel_num])
                    //         {
                    //             prefetch_mid_index = j;
                    //             find_prefetch = true;
                    //             break;
                    //         }
                    //     }
                    //     if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                    //     {
                    //         overall_doable = true;
                    //     }
                    // }

                    // if (!overall_doable)
                    // {
                    //     offload_mid_index = a->kernelLevel_interval[0];
                    //     prefetch_mid_index = needed_index;
                    //     a->gar_for_step2_offloading = true;
                    // }


                    //Optimization: quick check
                    if (offload_mid_index+1 < prefetch_mid_index && GPU_resident_memory_estimation[(offload_mid_index+1) % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > a->GPU_mem_line
                        && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line)
                    {
                        area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table_extended_sort[prefetch_mid_index] - kernel_time_table_extended_sort[offload_mid_index+1]);
                    }
                    else
                    {
                        for (int j = offload_mid_index+1; j < prefetch_mid_index; j++)
                        {
                            if (GPU_resident_memory_estimation[j % kernel_num] > a->GPU_mem_line)
                            {
                                area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
                            }
                        }
                    }

                    // if(!overall_doable)
                    // {
                    //     area_can_reduce_a = area_can_reduce_a * 0.5;
                    // }

                }
            }

            a->area_can_reduce = area_can_reduce_a;
        }
        


        //Sort the intervals
        std::sort(interval_list.begin(), interval_list.end(), [](Hidding_Interval* a, Hidding_Interval* b){
            double area_can_reduce_a = a->area_can_reduce;
            double area_can_reduce_b = b->area_can_reduce;

            // if (a->is_really_offloaded || a->the_tensor->size_in_byte < 1024*32)
            // {
            //     area_can_reduce_a = 0;
            // }
            // else if (b->is_really_offloaded || b->the_tensor->size_in_byte < 1024*32)
            // {
            //     area_can_reduce_a = 10;
            // }
            // else
            // {
            //     if (!(a->is_looped))
            //     {
            //         int offload_mid_index = -1;
            //         int prefetch_mid_index = -1;
            //         // offload_mid_index = a->kernelLevel_interval[0];
            //         // prefetch_mid_index = a->kernelLevel_interval[1];

            //         bool cpu_ok = check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1]);
            //         bool cpu_doable = false;
            //         bool overall_doable = false;

            //         if (cpu_ok)
            //         {
            //             bool find_offload = false;
            //             for (int j = a->kernelLevel_interval[0]; j < a->kernelLevel_interval[1]; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2C[j])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = a->kernelLevel_interval[1]; j > a->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_C2G[j])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 cpu_doable = true;
            //                 overall_doable = true;
            //             }
                        
            //         }

            //         if(!cpu_doable){
            //             bool find_offload = false;
            //             for (int j = a->kernelLevel_interval[0]; j < a->kernelLevel_interval[1]; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2S[j])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = a->kernelLevel_interval[1]; j > a->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_S2G[j])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 overall_doable = true;
            //             }
            //         }

            //         if (!overall_doable)
            //         {
            //             offload_mid_index = a->kernelLevel_interval[0];
            //             prefetch_mid_index = a->kernelLevel_interval[1];
            //             a->gar_for_step2_offloading = true;
            //         }
                    

            //         //Optimization: quick check
            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index] > a->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > a->GPU_mem_line
            //         && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > a->GPU_mem_line)
            //         {
            //             area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j] > a->GPU_mem_line)
            //                 {
            //                     area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
            //                 }
            //             }
            //         }

            //         if (!overall_doable)
            //         {
            //             area_can_reduce_a = area_can_reduce_a * 0.1;
            //         }

            //     }
            //     else
            //     {
            //         int offload_mid_index = -1;
            //         int prefetch_mid_index = -1;
            //         // offload_mid_index = a->kernelLevel_interval[0];
            //         // prefetch_mid_index = a->kernelLevel_interval[1] + kernel_num;


            //         bool cpu_ok = check_CPU_OK_interval(CPU_line - a->the_tensor->size_in_byte, a->kernelLevel_interval[0], a->kernelLevel_interval[1]);
            //         bool cpu_doable = false;
            //         bool overall_doable = false;
            //         int needed_index = a->kernelLevel_interval[1] + kernel_num;

            //         if (cpu_ok)
            //         {
            //             bool find_offload = false;
            //             for (int j = a->kernelLevel_interval[0]; j < needed_index; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2C[j%kernel_num])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = needed_index; j > a->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_C2G[j%kernel_num])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 cpu_doable = true;
            //                 overall_doable = true;
            //             }
                        
            //         }

            //         if(!cpu_doable){
            //             bool find_offload = false;
            //             for (int j = a->kernelLevel_interval[0]; j < needed_index; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2S[j%kernel_num])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = needed_index; j > a->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_S2G[j%kernel_num])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 overall_doable = true;
            //             }
            //         }

            //         if (!overall_doable)
            //         {
            //             offload_mid_index = a->kernelLevel_interval[0];
            //             prefetch_mid_index = needed_index;
            //             a->gar_for_step2_offloading = true;
            //         }


            //         //Optimization: quick check
            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > a->GPU_mem_line
            //             && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > a->GPU_mem_line)
            //         {
            //             area_can_reduce_a = a->the_tensor->size_in_byte * (kernel_time_table_extended_sort[prefetch_mid_index] - kernel_time_table_extended_sort[offload_mid_index]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j % kernel_num] > a->GPU_mem_line)
            //                 {
            //                     area_can_reduce_a += a->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
            //                 }
            //             }
            //         }

            //         if(!overall_doable)
            //         {
            //             area_can_reduce_a = area_can_reduce_a * 0.1;
            //         }

            //     }
            // }

            // if (b->is_really_offloaded || b->the_tensor->size_in_byte < 1024*32)
            // {
            //     area_can_reduce_b = 0;
            // }
            // else if (area_can_reduce_a == 0)
            // {
            //     area_can_reduce_b = 10;
            // }
            // else
            // {
            //     if (!(b->is_looped))
            //     {
            //         int offload_mid_index = -1;
            //         int prefetch_mid_index = -1;
            //         // offload_mid_index = b->kernelLevel_interval[0];
            //         // prefetch_mid_index = b->kernelLevel_interval[1];

            //         bool cpu_ok = check_CPU_OK_interval(CPU_line - b->the_tensor->size_in_byte, b->kernelLevel_interval[0], b->kernelLevel_interval[1]);
            //         bool cpu_doable = false;
            //         bool overall_doable = false;

            //         if (cpu_ok)
            //         {
            //             bool find_offload = false;
            //             for (int j = b->kernelLevel_interval[0]; j < b->kernelLevel_interval[1]; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2C[j])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = b->kernelLevel_interval[1]; j > b->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_C2G[j])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 cpu_doable = true;
            //                 overall_doable = true;
            //             }
                        
            //         }

            //         if(!cpu_doable){
            //             bool find_offload = false;
            //             for (int j = b->kernelLevel_interval[0]; j < b->kernelLevel_interval[1]; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2S[j])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = b->kernelLevel_interval[1]; j > b->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_S2G[j])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 overall_doable = true;
            //             }
            //         }

            //         if (!overall_doable)
            //         {
            //             offload_mid_index = b->kernelLevel_interval[0];
            //             prefetch_mid_index = b->kernelLevel_interval[1];
            //             b->gar_for_step2_offloading = true;
            //         }



            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index] > b->GPU_mem_line && GPU_resident_memory_estimation[prefetch_mid_index-1] > b->GPU_mem_line
            //             && GPU_resident_memory_estimation[offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3] > b->GPU_mem_line && GPU_resident_memory_estimation[offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3] > b->GPU_mem_line)
            //         {
            //             area_can_reduce_b = b->the_tensor->size_in_byte * (kernel_time_table[prefetch_mid_index] - kernel_time_table[offload_mid_index]);
            //         }
            //         else
            //         {
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j] > b->GPU_mem_line)
            //                 {
            //                     area_can_reduce_b += b->the_tensor->size_in_byte * (kernel_time_table[j+1] - kernel_time_table[j]);
            //                 }
            //             }
            //         }

            //         if(!overall_doable)
            //         {
            //             area_can_reduce_b = area_can_reduce_b * 0.1;
            //         }
            //     }
            //     else
            //     {
            //         int offload_mid_index = -1;
            //         int prefetch_mid_index = -1;
            //         // offload_mid_index = b->kernelLevel_interval[0];
            //         // prefetch_mid_index = b->kernelLevel_interval[1] + kernel_num;


            //         bool cpu_ok = check_CPU_OK_interval(CPU_line - b->the_tensor->size_in_byte, b->kernelLevel_interval[0], b->kernelLevel_interval[1]);
            //         bool cpu_doable = false;
            //         bool overall_doable = false;
            //         int needed_index = b->kernelLevel_interval[1] + kernel_num;

            //         if (cpu_ok)
            //         {
            //             bool find_offload = false;
            //             for (int j = b->kernelLevel_interval[0]; j < needed_index; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2C[j%kernel_num])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = needed_index; j > b->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_C2G[j%kernel_num])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 cpu_doable = true;
            //                 overall_doable = true;
            //             }
                        
            //         }

            //         if(!cpu_doable){
            //             bool find_offload = false;
            //             for (int j = b->kernelLevel_interval[0]; j < needed_index; j++)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_G2S[j%kernel_num])
            //                 {
            //                     offload_mid_index = j;
            //                     find_offload = true;
            //                     break;
            //                 }
            //             }
            //             bool find_prefetch = false;
            //             for (int j = needed_index; j > b->kernelLevel_interval[0]; j--)
            //             {
            //                 if (kernel_no_ongoing_mirgation_list_S2G[j%kernel_num])
            //                 {
            //                     prefetch_mid_index = j;
            //                     find_prefetch = true;
            //                     break;
            //                 }
            //             }
            //             if (find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
            //             {
            //                 overall_doable = true;
            //             }
            //         }

            //         if (!overall_doable)
            //         {
            //             offload_mid_index = b->kernelLevel_interval[0];
            //             prefetch_mid_index = needed_index;
            //             b->gar_for_step2_offloading = true;
            //         }


            //         if (offload_mid_index < prefetch_mid_index && GPU_resident_memory_estimation[offload_mid_index % kernel_num] > b->GPU_mem_line && GPU_resident_memory_estimation[(prefetch_mid_index-1) % kernel_num] > b->GPU_mem_line
            //             && GPU_resident_memory_estimation[(offload_mid_index + (prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > b->GPU_mem_line && GPU_resident_memory_estimation[(offload_mid_index + 2*(prefetch_mid_index-1-offload_mid_index)/3) % kernel_num] > b->GPU_mem_line)
            //         {
            //             area_can_reduce_b = b->the_tensor->size_in_byte * (kernel_time_table_extended_sort[prefetch_mid_index] - kernel_time_table_extended_sort[offload_mid_index]);
            //         }
            //         else
            //         {
                        
            //             for (int j = offload_mid_index; j < prefetch_mid_index; j++)
            //             {
            //                 if (GPU_resident_memory_estimation[j % kernel_num] > b->GPU_mem_line)
            //                 {
            //                     area_can_reduce_b += b->the_tensor->size_in_byte * (kernel_time_table[(j % kernel_num) + 1] - kernel_time_table[j % kernel_num]);
            //                 }
            //             }
            //         }

            //         if(!overall_doable){
            //             area_can_reduce_b = area_can_reduce_b * 0.1;
            //         }

            //     }
            // }


            bool ret;

            if (area_can_reduce_a != 0 && area_can_reduce_b != 0 && ((a->the_tensor->size_in_byte) - (b->the_tensor->size_in_byte) < 200000000) && ((a->the_tensor->size_in_byte) - (b->the_tensor->size_in_byte) > -200000000) && (a->the_tensor->size_in_byte) > 2000000 && (b->the_tensor->size_in_byte) > 2000000)
            {
                return (area_can_reduce_a / (a->the_tensor->size_in_byte)) > (area_can_reduce_b / (b->the_tensor->size_in_byte));
            }
            else 
            {
                return (area_can_reduce_a > area_can_reduce_b);
            }

            return ret;
        });


        bool need_to_break = false;

        for (int kk = 0; kk < Sample*5; kk++)
        {

        
            // The interval list is already sorted
            Hidding_Interval* curr_interval = interval_list[kk];

            if (curr_interval->is_really_offloaded)
            {
                need_to_break = true;
                break;
            }
            

            int cha;
            if (!curr_interval->is_looped)
            {
                cha = curr_interval->kernelLevel_interval[1] - curr_interval->kernelLevel_interval[0];
            }
            else
            {
                cha = curr_interval->kernelLevel_interval[1] + kernel_num - curr_interval->kernelLevel_interval[0];
            }

            if (cha==1)
            {
                continue;
            }


            // bool offload_to_cpu = false;
            bool offload_to_cpu = check_CPU_OK_interval(CPU_line - curr_interval->the_tensor->size_in_byte, curr_interval->kernelLevel_interval[0], curr_interval->kernelLevel_interval[1]);
            int offload_mid_index = curr_interval->kernelLevel_interval[0];
            int prefetch_mid_index = -1;

            if (!curr_interval->is_looped)  //Not looped
            {
                int needed_index = curr_interval->kernelLevel_interval[1];
                
                // if (cpu_ok)
                // {
                //     bool find_offload = true;
                //     // for (int j = curr_interval->kernelLevel_interval[0]; j < needed_index; j++)
                //     // {
                //     //     if (kernel_no_ongoing_mirgation_list_G2C[j])
                //     //     {
                //     //         offload_mid_index = j;
                //     //         find_offload = true;
                //     //         break;
                //     //     }
                //     // }
                //     bool find_prefetch = false;
                //     for (int j = needed_index; j > curr_interval->kernelLevel_interval[0]; j--)
                //     {
                //         if (kernel_no_ongoing_mirgation_list_C2G[j])
                //         {
                //             prefetch_mid_index = j;
                //             find_prefetch = true;
                //             break;
                //         }
                //     }
                //     if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                //     {
                //         offload_to_cpu = true;
                //     }
                //     else
                //     {
                //         offload_to_cpu = true;
                //         offload_mid_index = curr_interval->kernelLevel_interval[0];
                //         prefetch_mid_index = needed_index;
                //     }
                    
                // }
                // else{
                //     offload_mid_index = curr_interval->kernelLevel_interval[0];
                //     prefetch_mid_index = needed_index;
                //     if (!curr_interval->gar_for_step2_offloading)
                //     {
                //         bool find_offload = true;
                //         // for (int j = curr_interval->kernelLevel_interval[0]; j < needed_index; j++)
                //         // {
                //         //     if (kernel_no_ongoing_mirgation_list_G2S[j])
                //         //     {
                //         //         offload_mid_index = j;
                //         //         find_offload = true;
                //         //         break;
                //         //     }
                //         // }
                //         bool find_prefetch = false;
                //         for (int j = needed_index; j > curr_interval->kernelLevel_interval[0]; j--)
                //         {
                //             if (kernel_no_ongoing_mirgation_list_S2G[j])
                //             {
                //                 prefetch_mid_index = j;
                //                 find_prefetch = true;
                //                 break;
                //             }
                //         }
                //     }
                // }
                prefetch_mid_index = needed_index;

                Assert(offload_mid_index != -1);
                Assert(prefetch_mid_index != -1);


                DataMovementHint pre_evict(PageLocation::NOT_KNOWN, offload_to_cpu ? PageLocation::IN_CPU : PageLocation::IN_SSD, offload_mid_index, curr_interval->the_tensor);
                pre_evict.barrier_end_time = offload_mid_index;
                pre_evict.p_order = global_p_order_neg;
                global_p_order_neg--;
                movement_hints.push_back(pre_evict);
                curr_interval->the_tensor->is_choosed_to_evict = true;
                curr_interval->is_really_offloaded = true;
                //@Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;

                DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, prefetch_mid_index, curr_interval->the_tensor);
                pre_fetch.barrier_end_time = prefetch_mid_index;
                pre_fetch.p_order = global_p_order_neg;
                global_p_order_neg--;
                movement_hints.push_back(pre_fetch);
                //@Pre-fetch tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[1]-1<<std::endl;


                //minus mem
                for (int j = offload_mid_index+1; j < prefetch_mid_index; j++)
                {
                    GPU_resident_memory_estimation[j] -= curr_interval->the_tensor->size_in_byte;
                }
            }
            else
            {
                int needed_index = curr_interval->kernelLevel_interval[1] + kernel_num;
                
                // if (cpu_ok)
                // {
                //     bool find_offload = false;
                //     for (int j = curr_interval->kernelLevel_interval[0]; j < needed_index; j++)
                //     {
                //         if (kernel_no_ongoing_mirgation_list_G2C[j%kernel_num])
                //         {
                //             offload_mid_index = j;
                //             find_offload = true;
                //             break;
                //         }
                //     }
                //     bool find_prefetch = false;
                //     for (int j = needed_index; j > curr_interval->kernelLevel_interval[0]; j--)
                //     {
                //         if (kernel_no_ongoing_mirgation_list_C2G[j%kernel_num])
                //         {
                //             prefetch_mid_index = j;
                //             find_prefetch = true;
                //             break;
                //         }
                //     }
                //     if(find_offload && find_prefetch && offload_mid_index < prefetch_mid_index)
                //     {
                //         offload_to_cpu = true;
                //     }
                //     else
                //     {
                //         offload_to_cpu = true;
                //         offload_mid_index = curr_interval->kernelLevel_interval[0];
                //         prefetch_mid_index = needed_index;
                //     }
                    
                // }
                // else{
                //     offload_mid_index = curr_interval->kernelLevel_interval[0];
                //     prefetch_mid_index = needed_index;
                //     if (!curr_interval->gar_for_step2_offloading)
                //     {
                //         bool find_offload = false;
                //         for (int j = curr_interval->kernelLevel_interval[0]; j < needed_index; j++)
                //         {
                //             if (kernel_no_ongoing_mirgation_list_G2S[j%kernel_num])
                //             {
                //                 offload_mid_index = j;
                //                 find_offload = true;
                //                 break;
                //             }
                //         }
                //         bool find_prefetch = false;
                //         for (int j = needed_index; j > curr_interval->kernelLevel_interval[0]; j--)
                //         {
                //             if (kernel_no_ongoing_mirgation_list_S2G[j%kernel_num])
                //             {
                //                 prefetch_mid_index = j;
                //                 find_prefetch = true;
                //                 break;
                //             }
                //         }
                //     }
                // }
                prefetch_mid_index = needed_index;

                Assert(offload_mid_index != -1);
                Assert(prefetch_mid_index != -1);



                DataMovementHint pre_evict(PageLocation::NOT_KNOWN, offload_to_cpu ? PageLocation::IN_CPU : PageLocation::IN_SSD, offload_mid_index%kernel_num, curr_interval->the_tensor);
                pre_evict.barrier_end_time = offload_mid_index%kernel_num;
                pre_evict.p_order = global_p_order_neg;
                global_p_order_neg--;
                movement_hints.push_back(pre_evict);
                curr_interval->the_tensor->is_choosed_to_evict = true;
                curr_interval->is_really_offloaded = true;
                //@Pre-evict tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<curr_interval->kernelLevel_interval[0]<<std::endl;

                DataMovementHint pre_fetch(PageLocation::NOT_KNOWN, PageLocation::IN_GPU, (prefetch_mid_index)%kernel_num, curr_interval->the_tensor);
                pre_fetch.barrier_end_time = (prefetch_mid_index)%kernel_num;
                pre_fetch.p_order = global_p_order_neg;
                global_p_order_neg--;
                movement_hints.push_back(pre_fetch);
                //@Pre-fetch tensor "<<curr_interval->the_tensor->tensor_id<<" at kernel ID "<<(curr_interval->kernelLevel_interval[1] + kernel_num -1)%kernel_num<<std::endl;

                //minus mem
                for (int j = offload_mid_index+1; j < prefetch_mid_index; j++)
                {
                    GPU_resident_memory_estimation[j % kernel_num] -= curr_interval->the_tensor->size_in_byte;
                }

            }

            if (offload_to_cpu)
            {
                CPU_add_update_interval(curr_interval->the_tensor->size_in_byte, offload_mid_index, prefetch_mid_index);
            }
        }
        if (need_to_break)
        {
            std::cout<<"Need to break"<<std::endl;
            break;
        }
        
    }

    std::cerr << "After Tolerant offloading" << std::endl;
    print_GPU_mem_estimation("offloaded");

    for (int i = 0; i < interval_list.size(); i++)
    {

        if (!interval_list[i]->is_really_offloaded)
        {
            std::cout<<"Interval "<<i<<std::endl;        
            interval_list[i]->print();
        }
    }
    


    
    hill_mem = GPU_resident_memory_estimation[hill_index];

    // //Sort the intervals
    //  std::sort(interval_list.begin(), interval_list.end(), [](Hidding_Interval* a, Hidding_Interval* b){
    //      return (a->time_estimated * a->the_tensor->size_in_byte) > (b->time_estimated * b->the_tensor->size_in_byte);
    //     });


    // if (migration_policy_str=="G10GDSSSD" || migration_policy_str=="G10GDSFULL")
    // {
    //     // Cold periods final pass, for GDS setting
    //     for (int i = 0; i < interval_list.size(); i++)
    //     {
            
    //         if (hill_mem < target_mem_line*0.9)    //If already OK, end this loop
    //         {
    //             break;
    //         }
            
    //         // The interval list is already sorted
    //         Hidding_Interval* curr_interval = interval_list[i];

    //         if (curr_interval->is_really_offloaded)
    //         {
    //             continue;
    //         }

    //         if (!curr_interval->is_looped && curr_interval->kernelLevel_interval[0] <= hill_index && curr_interval->kernelLevel_interval[1] > hill_index)
    //         {
    //             //First schedule the pre-eviction
    //             DataMovementHint pre_evict(PageLocation::NOT_KNOWN, PageLocation::IN_SSD, curr_interval->kernelLevel_interval[0], curr_interval->the_tensor);
    //             movement_hints.push_back(pre_evict);
    //             curr_interval->the_tensor->is_choosed_to_evict = true;

    //             for (int j = curr_interval->kernelLevel_interval[0] + 1; j < curr_interval->kernelLevel_interval[1]; j++)
    //             {
    //                 GPU_resident_memory_estimation[j] -= curr_interval->the_tensor->size_in_byte;
    //             }
    //             hill_mem -= curr_interval->the_tensor->size_in_byte;
    //             curr_interval->is_really_offloaded = true;
    //         }
    //     }

    //     std::cerr<<"hill_index = "<<hill_index<<std::endl;

    //     // std::cerr << "---------After Final-time GDS Modification" << std::endl;
    //     // print_GPU_mem_estimation();
    // }
    

    // sort the prefetch instructions according to the kernel_id issued
        sort(movement_hints.begin(), movement_hints.end());

}

void print_prefetch_table(){
    for (int i = 0; i < movement_hints.size(); i++)
    {
        DataMovementHint curr = movement_hints[i];
        std::cout << "Issued Time: " << curr.issued_time << " ";
        std::cout << "Tensor: " << curr.tensor->tensor_id << " G:" << (curr.tensor->is_global_weight ? "o" : "x") << " ";
        std::cout << "From: " << Simulator::print_pagelocation_array[curr.from] << ", To: " << Simulator::print_pagelocation_array[curr.to] << std::endl;
        migration_plan_output<<curr.issued_time<<" "<<curr.tensor->tensor_id<<" "<<(curr.to == PageLocation::IN_GPU ? "1:0" : ( curr.to == PageLocation::IN_CPU ? "0:0" : "21:0"))<<" "<<curr.barrier_end_time<<std::endl;
    }
    
}


void print_GPU_mem_estimation(string addi){
    std::cout<<addi<<"_total = [";
    for (int i = 0; i < kernel_list.size(); i++)
    {
        std::cout<<GPU_resident_memory_estimation[i]<<",";
    }
    std::cout<<"]"<<std::endl;

    long max = 0;
    for (int i = 0; i < kernel_list.size(); i++)
    {
        if (GPU_resident_memory_estimation[i] > max)
        {
            /* code */
            max = GPU_resident_memory_estimation[i];
        }
    }
    std::cout<<addi<<"_max = "<<max<<std::endl;
}


void print_interation_time(){
    std::cout<<"iteration_time = "<<kernel_time_table[kernel_num]<<std::endl;
}


void Hidding_Interval::print(){
    std::cout<<"interval "<<": "<<kernelLevel_interval[0]<<"--------"<<kernelLevel_interval[1]<<std::endl;
    std::cout<<"Estimated Time:"<<time_estimated<<std::endl;
    std::cout<<"Tensor: ";
    this->the_tensor->print();
    std::cout<<"Is_offloaded?: "<<is_offloaded<<std::endl;
    std::cout<<"_______________________________________________________________"<<std::endl;
}



void print_GPU_mem_really_in_use(){
    for (int i = 0; i < kernel_list.size(); i++)
    {
        std::vector<Tensor*> r;
        kernel_list[i].getRequiredTensors(r);
        long size_bite = 0;
        for (int j = 0; j < r.size(); j++)
        {
            size_bite += r[j]->size_in_byte;
        }
        std::cout<<"Kernel "<<i<<": "<<size_bite<<std::endl;
    }
    
}


FlashNeuron_memory_manager::FlashNeuron_memory_manager(double GPUsize_GB){
    std::cout<<"Initializing FlashNeuron Memory manager..."<<std::endl;
    long page_number = (long)(GPUsize_GB * 1024 * 1024 / 4);
    page_table.resize(page_number);
    for (long i = 0; i < page_number; i++)
    {
        page_table[i].valid = false;
    }
    std::cout<<"Done!"<<std::endl;
}


double FlashNeuron_memory_manager::util_cal(){
    int u = 0;
    for (int i = 0; i < page_table.size(); i++)
    {
        if (page_table[i].valid)
        {
            u++;
        }
    }
    return ((double)u / (double)(page_table.size()));
}


//return 0: success, return 1: failed
int FlashNeuron_memory_manager::alloc_from_left(Tensor* tensor){
    long page_size =  std::ceil((float) tensor->size_in_byte / PAGE_SIZE);
    bool find = false;

    long index = 0;
    bool in_range_search = false;
    long in_range_size = 0;
    while (index < page_table.size())
    {
        if (page_table[index].valid && !in_range_search)
        {
            index++;
            continue;
        }
        else if (!page_table[index].valid && !in_range_search)
        {
            in_range_search = true;
            in_range_size = 1;
            index++;
        }
        else if (!page_table[index].valid && in_range_search)
        {
            in_range_size++;
            index++;
        }
        else if (page_table[index].valid && in_range_search)
        {
            in_range_size = 0;
            in_range_search = false;
            index++;
        }
        
        if (in_range_size == page_size)
        {
            find = true;
            break;
        }
    }
    if (!find)
    {
        return 1;
    }
    else
    {
        for (long i = index-1; i > index - page_size - 1; i--)
        {
            page_table[i].valid = true;
        }
        tensor->f_is_allocated_on_GPU = true;
        tensor->f_page_range[0] = index - page_size;
        tensor->f_page_range[1] = index;
        std::cout<<"Allocated tensor "<<tensor->name()<<" on GPU memory."<<std::endl;
        return 0;
    }
}




//return 0: success, return 1: failed
int FlashNeuron_memory_manager::alloc_from_right(Tensor* tensor){
    long page_size =  tensor->size_in_byte / 4096;
    bool find = false;

    long index = page_table.size()-1;
    bool in_range_search = false;
    long in_range_size = 0;
    while (index >= 0)
    {
        if (page_table[index].valid && !in_range_search)
        {
            index--;
            continue;
        }
        else if (!page_table[index].valid && !in_range_search)
        {
            in_range_search = true;
            in_range_size = 1;
            index--;
        }
        else if (!page_table[index].valid && in_range_search)
        {
            in_range_size++;
            index--;
        }
        else if (page_table[index].valid && in_range_search)
        {
            in_range_size = 0;
            in_range_search = false;
            index--;
        }
        
        if (in_range_size == page_size)
        {
            find = true;
            break;
        }
    }

    if (!find)
    {
        return 1;
    }
    else
    {
        for (long i = index+1; i < index + page_size +1; i++)
        {
            page_table[i].valid = true;
        }
        tensor->f_is_allocated_on_GPU = true;
        tensor->f_page_range[0] = index + 1;
        tensor->f_page_range[1] = index + page_size +1;
        std::cout<<"Allocated tensor "<<tensor->name()<<" on GPU memory."<<std::endl;
        return 0;
    }
}


long FlashNeuron_memory_manager::largest_available_size(){
    long index = 0;
    bool in_range_search = false;
    long in_range_size = 0;
    long max_range_size = 0;
    while (index < page_table.size())
    {
        if (page_table[index].valid && !in_range_search)
        {
            index++;
            continue;
        }
        else if (!page_table[index].valid && !in_range_search)
        {
            in_range_search = true;
            in_range_size = 1;
            index++;
        }
        else if (!page_table[index].valid && in_range_search)
        {
            in_range_size++;
            index++;
            if (index >= page_table.size())
            {
                if (in_range_size > max_range_size)
                {
                    max_range_size = in_range_size;
                }
            }
        }
        else if (page_table[index].valid && in_range_search)
        {
            if (in_range_size > max_range_size)
            {
                max_range_size = in_range_size;
            }
            in_range_size = 0;
            in_range_search = false;
            index++;
        }
    }
    return max_range_size;
}



void FlashNeuron_memory_manager::dealloc_tensor(Tensor* tensor){
    if (!tensor->f_is_allocated_on_GPU)
    {
        return;
    }
    
    Assert(tensor->f_is_allocated_on_GPU);
    tensor->f_is_allocated_on_GPU = false;
    Assert(tensor->f_page_range[1] > tensor->f_page_range[0]);
    for (long i = tensor->f_page_range[0]; i < tensor->f_page_range[1]; i++)
    {
        page_table[i].valid = false;
    }
    std::cout<<"De-allocated tensor "<<tensor->name()<<" on GPU memory."<<std::endl;
}

// For GDS-based settings, we do the mem size restriction at scheduling time, and use simplified memory manager
FlashNeuron_simulator::FlashNeuron_simulator(double BW_ssd_GBs, double BW_pcie_GBs, double GPU_size_GB, GDS_Baseline_Type type) : mem_manager(type==GDS_Baseline_Type::FlashNeuron ? GPU_size_GB : GPU_size_GB * 4) {
    total_sim_time = 0;
    total_trasfer_time = 0;
    total_time_breakdown_stall = 0;
    total_time_breakdown_overlap = 0;
    total_time_breakdown_exe = 0;
    total_offload_byte = 0;
    total_fetch_byte = 0;
    baseline_type = type;
    BW_ssd = BW_ssd_GBs * 1024 * 1024 * 1024 / 1000;
    BW_pcie = BW_pcie_GBs * 1024 * 1024 * 1024 / 1000;
    event_number = 0;
}


// return 0 for success, return 1 for failure, return 2 for kernel finished
int FlashNeuron_simulator::serve_one_pending_event(int kernel_event_id){
    Assert(!fl_pending_event_queue.empty());
    Assert(this->total_sim_time <= fl_pending_event_queue.top().ready_time);
    if (fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Kernel_Finish)
    {
        Assert(fl_pending_event_queue.top().event_id==kernel_event_id);
        if (!fl_fetch_queue.empty() || !fl_offload_queue.empty() || !fl_fetch_queue_cpu.empty() || !fl_offload_queue_cpu.empty())
        {
            this->total_trasfer_time += fl_pending_event_queue.top().ready_time - this->total_sim_time;
        }
        this->total_sim_time = fl_pending_event_queue.top().ready_time;
        fl_pending_event_queue.pop();
        return 2;
    }
    else if (fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Offload_Finish)
    {
        Assert(fl_pending_event_queue.top().event_id==fl_offload_queue.front().event_id);
        Assert(fl_offload_queue.front().is_happening);
        this->total_trasfer_time += fl_pending_event_queue.top().ready_time - this->total_sim_time;
        this->total_sim_time = fl_pending_event_queue.top().ready_time;
        fl_pending_event_queue.pop();

        std::cout<<"Time: "<<this->total_sim_time<<", Tensor: "<<fl_offload_queue.front().tensor->name()<<" finished offloading!"<<std::endl;
        this->mem_manager.dealloc_tensor(fl_offload_queue.front().tensor);
        fl_offload_queue.pop_front();
        if (!fl_offload_queue.empty())
        {
            fl_offload_queue.front().is_happening = true;
            fl_pending_event next_offload;
            next_offload.event_id = fl_offload_queue.front().event_id;
            next_offload.ready_time = this->total_sim_time + fl_offload_queue.front().estimated_time;
            next_offload.type = Fl_Pending_Event_Type::Offload_Finish;
            fl_pending_event_queue.push(next_offload);
        }
        return 0;
    }
    else if (fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Offload_Finish_CPU)
    {
        Assert(fl_pending_event_queue.top().event_id==fl_offload_queue_cpu.front().event_id);
        Assert(fl_offload_queue_cpu.front().is_happening);
        this->total_trasfer_time += fl_pending_event_queue.top().ready_time - this->total_sim_time;
        this->total_sim_time = fl_pending_event_queue.top().ready_time;
        fl_pending_event_queue.pop();

        std::cout<<"Time: "<<this->total_sim_time<<", Tensor: "<<fl_offload_queue_cpu.front().tensor->name()<<" finished offloading!"<<std::endl;
        this->mem_manager.dealloc_tensor(fl_offload_queue_cpu.front().tensor);
        cpu_tensors.insert(fl_offload_queue_cpu.front().tensor);
        std::cout<<"Tensor "<<fl_offload_queue_cpu.front().tensor->name()<<" is cpu tensor!"<<std::endl;
        fl_offload_queue_cpu.pop_front();
        if (!fl_offload_queue_cpu.empty())
        {
            fl_offload_queue_cpu.front().is_happening = true;
            fl_pending_event next_offload;
            next_offload.event_id = fl_offload_queue_cpu.front().event_id;
            next_offload.ready_time = this->total_sim_time + fl_offload_queue_cpu.front().estimated_time;
            next_offload.type = Fl_Pending_Event_Type::Offload_Finish_CPU;
            fl_pending_event_queue.push(next_offload);
        }
        return 0;
    }
    else if (fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Prefetch_Finish_CPU)
    {
        Assert(fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Prefetch_Finish_CPU);
        Assert(fl_pending_event_queue.top().event_id==fl_fetch_queue_cpu.front().event_id);
        Assert(fl_fetch_queue_cpu.front().is_happening);
        this->total_trasfer_time += fl_pending_event_queue.top().ready_time - this->total_sim_time;
        this->total_sim_time = fl_pending_event_queue.top().ready_time;
        fl_pending_event_queue.pop();

        std::cout<<"Time: "<<this->total_sim_time<<", Tensor: "<<fl_fetch_queue_cpu.front().tensor->name()<<" finished prefetching!"<<std::endl;
        fl_fetch_queue_cpu.front().tensor->f_is_fetching = false;
        Assert(cpu_tensors.find(fl_fetch_queue_cpu.front().tensor)!=cpu_tensors.end());
        cpu_tensors.erase(fl_fetch_queue_cpu.front().tensor);
        fl_fetch_queue_cpu.pop();
        if (!fl_fetch_queue_cpu.empty())
        {
            fl_fetch_queue_cpu.front().is_happening = true;
            fl_pending_event next_prefetch;
            next_prefetch.ready_time = this->total_sim_time + fl_fetch_queue_cpu.front().estimated_time;
            next_prefetch.event_id = fl_fetch_queue_cpu.front().event_id;
            next_prefetch.type = Fl_Pending_Event_Type::Prefetch_Finish_CPU;
            std::cout<<"Prefetching event for tensor "<< fl_fetch_queue_cpu.front().tensor->name()<<" is now pending!"<<std::endl;
            fl_pending_event_queue.push(next_prefetch);
        }
        return 0;
    }
    else  //Prefetch
    {
        Assert(fl_pending_event_queue.top().type==Fl_Pending_Event_Type::Prefetch_Finish);
        Assert(fl_pending_event_queue.top().event_id==fl_fetch_queue.front().event_id);
        Assert(fl_fetch_queue.front().is_happening);
        this->total_trasfer_time += fl_pending_event_queue.top().ready_time - this->total_sim_time;
        this->total_sim_time = fl_pending_event_queue.top().ready_time;
        fl_pending_event_queue.pop();

        std::cout<<"Time: "<<this->total_sim_time<<", Tensor: "<<fl_fetch_queue.front().tensor->name()<<" finished prefetching!"<<std::endl;
        fl_fetch_queue.front().tensor->f_is_fetching = false;
        fl_fetch_queue.pop();
        if (!fl_fetch_queue.empty())
        {
            fl_fetch_queue.front().is_happening = true;
            fl_pending_event next_prefetch;
            next_prefetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
            next_prefetch.event_id = fl_fetch_queue.front().event_id;
            next_prefetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
            fl_pending_event_queue.push(next_prefetch);
        }
        return 0;
    }
}


void FlashNeuron_simulator::check_fetch_allocation(int kernel_id){

    if (fetch_allocate_waiting_queue.empty())
    {
        return;
    }
    
    
    long curr_largest_avai_size = mem_manager.largest_available_size(); //granularity: page

    if (curr_largest_avai_size >= (fetch_allocate_waiting_queue.front().tensor->size_in_byte / 4096))
    {
        //schedule prefetch:
        int alloo = mem_manager.alloc_from_right(fetch_allocate_waiting_queue.front().tensor);
        Assert(alloo==0);

        fl_fetch pre_fetch;
        if (cpu_tensors.find(fetch_allocate_waiting_queue.front().tensor)!=cpu_tensors.end())
        {
            pre_fetch.estimated_time = fetch_allocate_waiting_queue.front().tensor->size_in_byte / ((CPU_PCIe_bandwidth_GBps - SSD_PCIe_bandwidth_GBps) * 1024 * 1024 * 1024 / 1000);
        }
        else
        {
            pre_fetch.estimated_time = fetch_allocate_waiting_queue.front().tensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
        }
        
        pre_fetch.event_id = event_number;
        event_number++;
        pre_fetch.tensor = fetch_allocate_waiting_queue.front().tensor;
        pre_fetch.is_happening = false;
        fetch_allocate_waiting_queue.front().tensor->f_is_fetching = true;

        if (cpu_tensors.find(fetch_allocate_waiting_queue.front().tensor)!=cpu_tensors.end()){
            fl_fetch_queue_cpu.push(pre_fetch);
            this->total_fetch_byte += pre_fetch.tensor->size_in_byte;
            std::cout<<"Prefetching event for cpu tensor "<< pre_fetch.tensor->name()<<" Scheduled!"<<std::endl;

            if (!fl_fetch_queue_cpu.front().is_happening)
            {
                fl_fetch_queue_cpu.front().is_happening = true;
                fl_pending_event pending_fetch;
                pending_fetch.event_id = fl_fetch_queue_cpu.front().event_id;
                pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue_cpu.front().estimated_time;
                pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish_CPU;
                std::cout<<"Prefetching event for tensor "<< fl_fetch_queue_cpu.front().tensor->name()<<" is now pending!"<<std::endl;
                fl_pending_event_queue.push(pending_fetch);
            }
        }
        else{
            fl_fetch_queue.push(pre_fetch);
            this->total_fetch_byte += pre_fetch.tensor->size_in_byte;
            std::cout<<"Prefetching event for tensor "<< pre_fetch.tensor->name()<<" Scheduled!"<<std::endl;
            // migration_plan_output<<"#Prefetching event for tensor "<< pre_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<kernel_id<<std::endl;
            migration_plan_output<<kernel_id<<" "<<pre_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

            if (!fl_fetch_queue.front().is_happening)
            {
                fl_fetch_queue.front().is_happening = true;
                fl_pending_event pending_fetch;
                pending_fetch.event_id = fl_fetch_queue.front().event_id;
                pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                fl_pending_event_queue.push(pending_fetch);
            }
        }

        fetch_allocate_waiting_tensors.erase(fetch_allocate_waiting_queue.front().tensor);
        fetch_allocate_waiting_queue.pop();
    }
}


void FlashNeuron_simulator::run(){

    long max_num_pages = 0;
    if (baseline_type==GDS_Baseline_Type::FlashNeuron)
    {
        // First do system initializations:
        
        for (auto it = kernel_list.begin(); it != kernel_list.end(); ++it) {
            CUDAKernel *current_kernel = &(*it);
            vector<Tensor *> required_tensors;
            current_kernel->getRequiredTensors(required_tensors);
            long num_pages = 0;
            for (Tensor *tensor : required_tensors) {
                if (!tensor->is_global_weight)
                {
                    num_pages += (tensor->size_in_byte / PAGE_SIZE);
                }
            }
            if (num_pages > max_num_pages) {
                max_num_pages = num_pages;
            }
        }

        std::cout<<"Initializaing the simulaton..."<<std::endl;
        // Allocate space for global tensors
        std::cout<<"Allocating space for global tensors..."<<std::endl;
        for (int i = 0; i < tensor_list.size(); i++)
        {
            if (tensor_list[i]->is_global_weight && tensor_list[i]->size_in_byte > 0)
            {
                int allo = mem_manager.alloc_from_left(tensor_list[i]);
                Assert(allo==0);
            }
        }
        std::cout<<"Done!"<<std::endl;

        //Reserve space for the largest workspace
        std::cout<<"Reserving space for the largest workspace tensors..."<<std::endl;
        Tensor* largest_wp_tensor = nullptr;
        long max_size = 0;
        for (int i = 0; i < tensor_list.size(); i++)
        {
            if (!tensor_list[i]->is_global_weight && tensor_list[i]->live_interval[1]==-1 && tensor_list[i]->size_in_byte > 0)
            {
                if (tensor_list[i]->size_in_byte > max_size)
                {
                    max_size = tensor_list[i]->size_in_byte;
                    largest_wp_tensor = tensor_list[i];
                }
            }
        }
        
        if (largest_wp_tensor)
        {
            int allo = mem_manager.alloc_from_left(largest_wp_tensor);
            Assert(allo==0);
        }
    }
    else
    {
        std::cout<<"Initializaing the simulaton..."<<std::endl;
        // Allocate space for global tensors
        std::cout<<"Allocating space for keep-on-GPU tensors..."<<std::endl;
        for (int i = 0; i < tensor_list.size(); i++)
        {
            if (tensor_list[i]->size_in_byte > 0 && tensor_list[i]->is_global_weight && !tensor_list[i]->is_choosed_to_evict)
            {
                int allo = mem_manager.alloc_from_left(tensor_list[i]);
                Assert(allo==0);
            }
            else if (tensor_list[i]->size_in_byte > 0 && tensor_list[i]->is_global_weight)
            {
                int allo = mem_manager.alloc_from_right(tensor_list[i]);
                Assert(allo==0);
            }
            
        }
    }
    
    std::cout<<"Done!"<<std::endl;
    
    //Start running
    std::cout<<"DNN training start!"<<std::endl;
    bool backward_starts = false;
    int offload_list_index = 0;
    int movement_list_index = 0;

    double ideal_exe_time = 0;

    double kernel_start_time;
    double kernel_finish_time;
    
    for (int i = 0; i < kernel_list.size(); i++)
    {
        if (kernel_list[i].type==CUDAKernelType::makeLoss)
        {
            backward_starts = true;
        }
        kernel_start_time = this->total_sim_time;

        std::cout<<"Time: "<<this->total_sim_time<<", "<<"Start to check the need tensors of kernel: "<< i <<" "<<print_kerneltype_array[kernel_list[i].type]<<std::endl;

        if (!backward_starts) //forward
        {
            // First check that if before this kernel we need to offload  
            if (baseline_type==GDS_Baseline_Type::FlashNeuron)
            {
                while (offload_list_index < offload_hints_fn.size())
                {
                    Assert(offload_hints_fn[offload_list_index].issued_time>=i);
                    if (offload_hints_fn[offload_list_index].issued_time==i)
                    {

                        fl_offload offload_event;
                        offload_event.event_id = event_number;
                        event_number++;
                        offload_event.is_happening = false;
                        offload_event.tensor = offload_hints_fn[offload_list_index].tensor;
                        offload_event.estimated_time = offload_event.tensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);

                        fl_offload_queue.push_back(offload_event);
                        this->total_offload_byte += offload_event.tensor->size_in_byte;
                        std::cout<<"Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<std::endl;
                        // migration_plan_output<<"#Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                        migration_plan_output<<i<<" "<<offload_event.tensor->tensor_id<<" "<<"21:0"<<std::endl;

                        if (!fl_offload_queue.front().is_happening)
                        {
                            fl_offload_queue.front().is_happening = true;
                            fl_pending_event pending_offload;
                            pending_offload.event_id = fl_offload_queue.front().event_id;
                            pending_offload.type = Fl_Pending_Event_Type::Offload_Finish;
                            pending_offload.ready_time = this->total_sim_time + fl_offload_queue.front().estimated_time;
                            fl_pending_event_queue.push(pending_offload);
                        }
                        offload_list_index++;
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }
        else {
            if (baseline_type==GDS_Baseline_Type::FlashNeuron)
            {
                //Backward starts, must wait until all offload finishes
                while (!fl_offload_queue.empty())
                {
                    int serve = this->serve_one_pending_event(0);
                    Assert(serve!=2 && serve!=1);
                }
            }
        }

        if (baseline_type!=GDS_Baseline_Type::FlashNeuron)
        { // It's GDS mode for G10
            while (movement_list_index < movement_hints.size())
            {
                Assert(movement_hints[movement_list_index].issued_time >= i);
                if (movement_hints[movement_list_index].issued_time==i)
                {
                    if (movement_hints[movement_list_index].to == PageLocation::IN_SSD) 
                    {
                        fl_offload offload_event;
                        offload_event.event_id = event_number;
                        event_number++;
                        offload_event.is_happening = false;
                        offload_event.tensor = movement_hints[movement_list_index].tensor;
                        offload_event.estimated_time = offload_event.tensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000); 

                        fl_offload_queue.push_back(offload_event);
                        this->total_offload_byte += offload_event.tensor->size_in_byte;
                        std::cout<<"Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<std::endl;
                        // migration_plan_output<<"#Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                        migration_plan_output<<i<<" "<<offload_event.tensor->tensor_id<<" "<<"21:0"<<std::endl;
                        if (!fl_offload_queue.front().is_happening)
                        {
                            fl_offload_queue.front().is_happening = true;
                            fl_pending_event pending_offload;
                            pending_offload.event_id = fl_offload_queue.front().event_id;
                            pending_offload.type = Fl_Pending_Event_Type::Offload_Finish;
                            pending_offload.ready_time = this->total_sim_time + fl_offload_queue.front().estimated_time;
                            fl_pending_event_queue.push(pending_offload);
                        }
                        movement_list_index++;
                    }
                    else if (movement_hints[movement_list_index].to == PageLocation::IN_CPU)
                    {
                        fl_offload offload_event;
                        offload_event.event_id = event_number;
                        event_number++;
                        offload_event.is_happening = false;
                        offload_event.tensor = movement_hints[movement_list_index].tensor;
                        offload_event.estimated_time = offload_event.tensor->size_in_byte / ((CPU_PCIe_bandwidth_GBps - SSD_PCIe_bandwidth_GBps) * 1024 * 1024 * 1024 / 1000);

                        fl_offload_queue_cpu.push_back(offload_event);
                        cpu_tensors.insert(fl_offload_queue_cpu.back().tensor);
                        this->total_offload_byte += offload_event.tensor->size_in_byte;
                        std::cout<<"Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<std::endl;
                        // migration_plan_output<<"#Offloading event for tensor "<< offload_event.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                        migration_plan_output<<i<<" "<<offload_event.tensor->tensor_id<<" "<<"21:0"<<std::endl;
                        if (!fl_offload_queue_cpu.front().is_happening)
                        {
                            fl_offload_queue_cpu.front().is_happening = true;
                            fl_pending_event pending_offload;
                            pending_offload.event_id = fl_offload_queue_cpu.front().event_id;
                            pending_offload.type = Fl_Pending_Event_Type::Offload_Finish_CPU;
                            pending_offload.ready_time = this->total_sim_time + fl_offload_queue_cpu.front().estimated_time;
                            fl_pending_event_queue.push(pending_offload);
                        }
                        movement_list_index++;
                    }
                    else if(movement_hints[movement_list_index].to == PageLocation::IN_GPU)
                    {
                        //prefetch
                        if (!(movement_hints[movement_list_index].tensor->f_is_allocated_on_GPU && movement_hints[movement_list_index].tensor->is_global_weight))
                        {
                            fetch_wait fw;
                            fw.tensor = movement_hints[movement_list_index].tensor;
                            fw.source = Simulator::PageLocation::IN_SSD; //TODO: 
                            fetch_allocate_waiting_queue.push(fw);
                            fetch_allocate_waiting_tensors.insert(fw.tensor);
                            std::cout<<"Inserted Waiting Prefetch: "<<fw.tensor->name()<<std::endl;
                            this->check_fetch_allocation(i);
                        }
                        movement_list_index++;
                    }
                    else
                    {
                        std::cout<<"Error: No GPU in the migration."<<std::endl;
                        return;
                    }
                    
                }
                else
                {
                    break;
                }
            }
        }

        //Then check and allocate all the intermeidate tensor needed
        CUDAKernel curr_kernel = kernel_list[i];
        vector<Tensor *> required_tensors;
        curr_kernel.getRequiredTensors(required_tensors);
        if (baseline_type==GDS_Baseline_Type::FlashNeuron)
        {
            for (auto tttensor : required_tensors)
            {
                if (!tttensor->is_global_weight && tttensor->live_interval[1]!=-1)
                {
                    if (!tttensor->f_is_allocated_on_GPU)
                    {
                        if (tttensor->f_is_choosed_to_offload)
                        {
                            int tryit = mem_manager.alloc_from_right(tttensor);
                            if(tttensor->name()=="tensor24"){
                                tryit = tryit;
                                tryit = 0;
                            }
                            if (tryit==1)
                            {
                                //No enough space
                                while (!fl_pending_event_queue.empty())
                                {
                                    int serve = this->serve_one_pending_event(0);
                                    Assert(serve!=2);
                                    int alloc_try = mem_manager.alloc_from_right(tttensor);
                                    if (alloc_try==0)
                                    {
                                        break;
                                    }
                                }
                                if (!tttensor->f_is_allocated_on_GPU)
                                {
                                    std::cout<<"No enough space to allocate tensor, G!"<<std::endl;
                                    std::cout<<"Utilization is: "<<mem_manager.util_cal()<<std::endl;
                                    exit(1);
                                }
                                
                                if (tttensor->live_interval[0]!=i) //the tensor is already alive, still need fetch
                                {
                                    fl_fetch on_d_fetch;
                                    on_d_fetch.estimated_time = tttensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
                                    on_d_fetch.event_id = event_number;
                                    event_number++;
                                    on_d_fetch.tensor = tttensor;
                                    on_d_fetch.is_happening = false;
                                    tttensor->f_is_fetching = true;
                                    fl_fetch_queue.push(on_d_fetch);
                                    this->total_fetch_byte += on_d_fetch.tensor->size_in_byte;
                                    std::cout<<"Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<std::endl;
                                    // migration_plan_output<<"#Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                                    migration_plan_output<<i<<" "<<on_d_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

                                    if (!fl_fetch_queue.front().is_happening)
                                    {
                                        fl_fetch_queue.front().is_happening = true;
                                        fl_pending_event pending_fetch;
                                        pending_fetch.event_id = fl_fetch_queue.front().event_id;
                                        pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                                        pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                                        fl_pending_event_queue.push(pending_fetch);
                                    }
                                    
                                    while (!fl_pending_event_queue.empty())
                                    {
                                        int serve = this->serve_one_pending_event(0);
                                        Assert(serve!=2);
                                        if (tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU)
                                        {
                                            break;
                                        }
                                    }
                                    Assert(tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU);
                                    
                                }
                                
                                
                            }
                            else
                            {
                                if (tttensor->live_interval[0]!=i) //the tensor is already alive, still need fetch
                                {
                                    fl_fetch on_d_fetch;
                                    on_d_fetch.estimated_time = tttensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
                                    on_d_fetch.event_id = event_number;
                                    event_number++;
                                    on_d_fetch.tensor = tttensor;
                                    on_d_fetch.is_happening = false;
                                    tttensor->f_is_fetching = true;
                                    fl_fetch_queue.push(on_d_fetch);
                                    this->total_fetch_byte += on_d_fetch.tensor->size_in_byte;
                                    std::cout<<"Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<std::endl;
                                    // migration_plan_output<<"#Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                                    migration_plan_output<<i<<" "<<on_d_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

                                    if (!fl_fetch_queue.front().is_happening)
                                    {
                                        fl_fetch_queue.front().is_happening = true;
                                        fl_pending_event pending_fetch;
                                        pending_fetch.event_id = fl_fetch_queue.front().event_id;
                                        pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                                        pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                                        fl_pending_event_queue.push(pending_fetch);
                                    }
                                    
                                    while (!fl_pending_event_queue.empty())
                                    {
                                        int serve = this->serve_one_pending_event(0);
                                        Assert(serve!=2);
                                        if (tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU)
                                        {
                                            break;
                                        }
                                    }
                                    Assert(tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU);
                                    
                                }
                            }
                        }
                        else
                        {
                            int tryit = mem_manager.alloc_from_left(tttensor);
                            if (tryit==1)
                            {
                                //No enough space
                                while (!fl_pending_event_queue.empty())
                                {
                                    int serve = this->serve_one_pending_event(0);
                                    Assert(serve!=2);
                                    int alloc_try = mem_manager.alloc_from_left(tttensor);
                                    if (alloc_try==0)
                                    {
                                        break;
                                    }
                                }
                                if (!tttensor->f_is_allocated_on_GPU)
                                {
                                    std::cout<<"No enough space to allocate tensor, G!"<<std::endl;
                                    std::cout<<"Utilization is: "<<mem_manager.util_cal()<<std::endl;
                                    exit(1);
                                }
                            }
                        }
                    }
                    else if (tttensor->f_is_allocated_on_GPU && tttensor->f_is_fetching)
                    {
                        while (!fl_pending_event_queue.empty())
                        {
                            int serve = this->serve_one_pending_event(0);
                            Assert(serve!=2);
                            if (tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU)
                            {
                                break;
                            }
                        }
                        Assert(tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU);
                    }
                }
            }
        }
        else{// GDS setting of G10
            for (auto tttensor : required_tensors)
            {
                if (!tttensor->f_is_allocated_on_GPU)
                {
                    if (fetch_allocate_waiting_tensors.find(tttensor)!=fetch_allocate_waiting_tensors.end()) //This tensor is waiting for allocate
                    {
                        check_fetch_allocation(i);
                        while (!fl_pending_event_queue.empty())
                        {
                            int serve = this->serve_one_pending_event(0);
                            Assert(serve!=2);
                            check_fetch_allocation(i);
                            if (tttensor->f_is_allocated_on_GPU)
                            {
                                break;
                            }
                        }
                        Assert(fetch_allocate_waiting_tensors.find(tttensor)==fetch_allocate_waiting_tensors.end());
                    }
                    else if (tttensor->is_choosed_to_evict)
                    {
                        int tryit = mem_manager.alloc_from_right(tttensor);
                        if(tttensor->name()=="tensor24"){
                            tryit = tryit;
                            tryit = 0;
                        }
                        if (tryit==1)
                        {
                            //No enough space
                            while (!fl_pending_event_queue.empty())
                            {
                                int serve = this->serve_one_pending_event(0);
                                Assert(serve!=2);
                                int alloc_try = mem_manager.alloc_from_right(tttensor);
                                if (alloc_try==0)
                                {
                                    check_fetch_allocation(i);
                                    break;
                                }
                            }
                            if (!tttensor->f_is_allocated_on_GPU)
                            {
                                std::cout<<"No enough space to allocate tensor, G!"<<std::endl;
                                std::cout<<"Utilization is: "<<mem_manager.util_cal()<<std::endl;
                                exit(1);
                            }
                            
                            if (tttensor->live_interval[0]!=i && !tttensor->is_global_weight) //the tensor is already alive, still need fetch
                            {
                                fl_fetch on_d_fetch;
                                on_d_fetch.estimated_time = tttensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
                                on_d_fetch.event_id = event_number;
                                event_number++;
                                on_d_fetch.tensor = tttensor;
                                on_d_fetch.is_happening = false;
                                tttensor->f_is_fetching = true;
                                fl_fetch_queue.push(on_d_fetch);
                                this->total_fetch_byte += on_d_fetch.tensor->size_in_byte;
                                std::cout<<"Fetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<std::endl;
                                // migration_plan_output<<"#Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                                migration_plan_output<<i<<" "<<on_d_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

                                if (!fl_fetch_queue.front().is_happening)
                                {
                                    fl_fetch_queue.front().is_happening = true;
                                    fl_pending_event pending_fetch;
                                    pending_fetch.event_id = fl_fetch_queue.front().event_id;
                                    pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                                    pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                                    fl_pending_event_queue.push(pending_fetch);
                                }
                                
                                while (!fl_pending_event_queue.empty())
                                {
                                    int serve = this->serve_one_pending_event(0);
                                    check_fetch_allocation(i);
                                    Assert(serve!=2);
                                    if (tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU)
                                    {
                                        break;
                                    }
                                }
                                Assert(tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU);
                                
                            }
                            
                            
                        }
                        else
                        {
                            if (tttensor->live_interval[0]!=i && !tttensor->is_global_weight) //the tensor is already alive, still need fetch
                            {
                                fl_fetch on_d_fetch;
                                on_d_fetch.estimated_time = tttensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
                                on_d_fetch.event_id = event_number;
                                event_number++;
                                on_d_fetch.tensor = tttensor;
                                on_d_fetch.is_happening = false;
                                tttensor->f_is_fetching = true;
                                fl_fetch_queue.push(on_d_fetch);
                                this->total_fetch_byte += on_d_fetch.tensor->size_in_byte;
                                std::cout<<"Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<std::endl;
                                // migration_plan_output<<"#Prefetching event for tensor "<< on_d_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                                migration_plan_output<<i<<" "<<on_d_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

                                if (!fl_fetch_queue.front().is_happening)
                                {
                                    fl_fetch_queue.front().is_happening = true;
                                    fl_pending_event pending_fetch;
                                    pending_fetch.event_id = fl_fetch_queue.front().event_id;
                                    pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                                    pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                                    fl_pending_event_queue.push(pending_fetch);
                                }
                                
                                while (!fl_pending_event_queue.empty())
                                {
                                    int serve = this->serve_one_pending_event(0);
                                    check_fetch_allocation(i);
                                    Assert(serve!=2);
                                    if (tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU)
                                    {
                                        break;
                                    }
                                }
                                Assert(tttensor->f_is_fetching==false && tttensor->f_is_allocated_on_GPU);
                                
                            }
                        }
                    }
                    else
                    {
                        int tryit = mem_manager.alloc_from_left(tttensor);
                        if (tryit==1)
                        {
                            //No enough space
                            while (!fl_pending_event_queue.empty())
                            {
                                int serve = this->serve_one_pending_event(0);
                                Assert(serve!=2);
                                int alloc_try = mem_manager.alloc_from_left(tttensor);
                                if (alloc_try==0)
                                {
                                    check_fetch_allocation(i);
                                    break;
                                }
                            }
                            if (!tttensor->f_is_allocated_on_GPU)
                            {
                                std::cout<<"No enough space to allocate tensor, G!"<<std::endl;
                                std::cout<<"Utilization is: "<<mem_manager.util_cal()<<std::endl;
                                exit(1);
                            }
                        }
                    }
                }
                else if (tttensor->f_is_allocated_on_GPU && tttensor->f_is_fetching)
                {
                    while (!fl_pending_event_queue.empty())
                    {
                        int serve = this->serve_one_pending_event(0);
                        check_fetch_allocation(i);
                        Assert(serve!=2);
                        if (tttensor->f_is_fetching==false)
                        {
                            break;
                        }
                    }
                    Assert(tttensor->f_is_fetching==false);
                }
                // else if (tttensor->f_is_allocated_on_GPU){
                //     bool is_offloading = false;
                //     for (auto it : fl_offload_queue)
                //     {
                //         if (it.tensor == tttensor)
                //         {
                //             is_offloading = true;
                //         }
                //     }
                //     if (is_offloading)
                //     {
                //         while (!fl_pending_event_queue.empty())
                //         {
                //             int serve = this->serve_one_pending_event(0);
                //             check_fetch_allocation(i);
                //             Assert(serve!=2);
                //             bool is_offloading = false;
                //             for (auto it : fl_offload_queue)
                //             {
                //                 if (it.tensor == tttensor)
                //                 {
                //                     is_offloading = true;
                //                 }
                //             }
                //             if (!is_offloading)
                //             {
                //                 break;
                //             }
                //         }
                //         is_offloading = false;
                //         for (auto it : fl_offload_queue)
                //         {
                //             if (it.tensor == tttensor)
                //             {
                //                 is_offloading = true;
                //             }
                //         }
                //         Assert(!is_offloading);
                //     }
                //     bool is_offloading_cpu = false;
                //     for (auto it : fl_offload_queue_cpu)
                //     {
                //         if (it.tensor == tttensor)
                //         {
                //             is_offloading_cpu = true;
                //         }
                //     }
                //     if (is_offloading_cpu)
                //     {
                //         while (!fl_pending_event_queue.empty())
                //         {
                //             int serve = this->serve_one_pending_event(0);
                //             check_fetch_allocation(i);
                //             Assert(serve!=2);
                //             bool is_offloading_cpu = false;
                //             for (auto it : fl_offload_queue_cpu)
                //             {
                //                 if (it.tensor == tttensor)
                //                 {
                //                     is_offloading_cpu = true;
                //                 }
                //             }
                //             if (!is_offloading_cpu)
                //             {
                //                 break;
                //             }
                //         }
                //         is_offloading_cpu = false;
                //         for (auto it : fl_offload_queue_cpu)
                //         {
                //             if (it.tensor == tttensor)
                //             {
                //                 is_offloading_cpu = true;
                //             }
                //         }
                //         Assert(!is_offloading_cpu);
                //     }
                // }
            }
        }
        

        std::cout<<"Time: "<<this->total_sim_time<<", Now the tensors for this kernel are all ready!"<<std::endl;


        //If OK, schedule the kernel execution
        double GPU_frequency_Hz = GPU_frequency_GHz * pow(10, 9);
        double kernel_exe_time = (double)curr_kernel.execution_cycles / GPU_frequency_Hz * 1000;
        ideal_exe_time += kernel_exe_time;
        fl_pending_event kernel;
        kernel.event_id = event_number;
        event_number++;
        kernel.ready_time = this->total_sim_time + kernel_exe_time;
        kernel.type = Fl_Pending_Event_Type::Kernel_Finish;
        fl_pending_event_queue.push(kernel);

        while (!fl_pending_event_queue.empty())
        {
            int serve = serve_one_pending_event(kernel.event_id);
            check_fetch_allocation(i+1);
            if (serve==2)
            {
                std::cout<<"Kernel no."<<i<<" "<<print_kerneltype_array[kernel_list[i].type]<<" has finished execution!"<<std::endl;
                break;
            }
        }
        

        //After one kernel finishes, check and deallocate dead tensors
        if (baseline_type==GDS_Baseline_Type::FlashNeuron)
        {
            for (auto tttensor : required_tensors){
                if (!tttensor->is_global_weight && tttensor->live_interval[1]!=-1){
                    if (tttensor->live_interval[1]==i+1)
                    {
                        mem_manager.dealloc_tensor(tttensor);
                    }
                }
            }
        }
        else{ //GDS setting for G10
            for (auto tttensor : required_tensors){
                if (!tttensor->is_global_weight){
                    if (tttensor->live_interval[1]==i+1 || tttensor->live_interval[1]==-1)
                    {
                        mem_manager.dealloc_tensor(tttensor);
                    }
                }
            }
            check_fetch_allocation(i+1);
        }
        
        
        if (baseline_type==GDS_Baseline_Type::FlashNeuron)
        {
            if (backward_starts)  //Backward, schedule prefetching
            { 
                long curr_largest_avai_size = mem_manager.largest_available_size(); //granularity: page
                long rest = curr_largest_avai_size - max_num_pages;
                int curr_kernel_index = i+1;
                bool stop = false;

                while (curr_kernel_index < kernel_list.size())
                {
                    CUDAKernel curr_kernel = kernel_list[curr_kernel_index];
                    vector<Tensor *> required_tensors;
                    curr_kernel.getRequiredTensors(required_tensors);
                    for (auto tttensor : required_tensors){
                        if (tttensor->f_is_choosed_to_offload && !tttensor->f_is_allocated_on_GPU)
                        {
                            if (rest > (tttensor->size_in_byte / 4096))
                            {
                                //schedule prefetch:
                                int alloo = mem_manager.alloc_from_right(tttensor);
                                Assert(alloo==0);

                                fl_fetch pre_fetch;
                                pre_fetch.estimated_time = tttensor->size_in_byte / (SSD_PCIe_bandwidth_GBps * 1024 * 1024 * 1024 / 1000);
                                pre_fetch.event_id = event_number;
                                event_number++;
                                pre_fetch.tensor = tttensor;
                                pre_fetch.is_happening = false;
                                tttensor->f_is_fetching = true;
                                fl_fetch_queue.push(pre_fetch);
                                this->total_fetch_byte += pre_fetch.tensor->size_in_byte;
                                std::cout<<"Prefetching event for tensor "<< pre_fetch.tensor->name()<<" Scheduled!"<<std::endl;
                                // migration_plan_output<<"#Prefetching event for tensor "<< pre_fetch.tensor->name()<<" Scheduled!"<<" at kernel id: "<<i<<std::endl;
                                migration_plan_output<<i<<" "<<pre_fetch.tensor->tensor_id<<" "<<"1:0"<<std::endl;

                                if (!fl_fetch_queue.front().is_happening)
                                {
                                    fl_fetch_queue.front().is_happening = true;
                                    fl_pending_event pending_fetch;
                                    pending_fetch.event_id = fl_fetch_queue.front().event_id;
                                    pending_fetch.ready_time = this->total_sim_time + fl_fetch_queue.front().estimated_time;
                                    pending_fetch.type = Fl_Pending_Event_Type::Prefetch_Finish;
                                    fl_pending_event_queue.push(pending_fetch);
                                }

                                rest -= (tttensor->size_in_byte / 4096);

                            }
                            else
                            {
                                stop = true;
                                break;
                            }
                        }
                    }
                    if (stop)
                    {
                        break;
                    }
                    curr_kernel_index++;
                }
            }
        }
        
        
        kernel_finish_time = this->total_sim_time;
        this->fl_kernel_stall_normed.push_back((kernel_finish_time-kernel_start_time-kernel_exe_time)/kernel_exe_time);
    }
    this->total_time_breakdown_stall = this->total_sim_time - ideal_exe_time;
    this->total_time_breakdown_overlap = this->total_trasfer_time - this->total_time_breakdown_stall;
    this->total_time_breakdown_exe = ideal_exe_time - this->total_time_breakdown_overlap;

    gpu2pcie_BW_estimation.resize(0);

}


// std::vector<std::string> split(const 't'::string s, const std::string delimiter) {
//     std::string scopy = s;
//     std::vector<std::string> result;
//     size_t pos = 0;
//     std::string token;
//     while ((pos = scopy.find(delimiter)) != std::string::npos) {
//         token = scopy.substr(0, pos);
//         scopy.erase(0, pos + delimiter.length());
//         result.push_back(token);
//     }
//     result.push_back(scopy);
//     return result;
// }


// Function to split a string by a delimiter
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}


void parse_temperal(std::string input_file){
    std::ifstream fin(input_file);
    std::string line;
    std::vector<std::string> i_tokens;
    std::unordered_set<int> global_tensor_ids;
    long iter_time = 0;
    fin>>line;
    fin>>line;
    i_tokens = split(line, ',');
    for (size_t i = 0; i < i_tokens.size(); i++)
    {
        global_tensor_ids.insert(std::stoi(i_tokens[i]));
    }

    int n_tensors;
    fin>>n_tensors;

    std::vector<std::vector<int>> tensor_activeness;
    tensor_activeness.resize(n_tensors);

    for (size_t i = 0; i < n_tensors; i++)
    {
        std::string line;
        fin>>line;
        std::cout<<line<<std::endl;
        std::vector<std::string> i_terms = split(line, '|');
        int tensor_id = std::stoi(i_terms[0]);
        long size = std::stol(i_terms[1]);

        std::vector<std::string> live_terms = split(i_terms[2], ',');
        bool is_meta_tensor = false;
        for (size_t j = 0; j < live_terms.size(); j++)
        {
            std::vector<std::string> activeness_terms = split(live_terms[j], ':');
            
            if (activeness_terms[1]!="1")
            {
                is_meta_tensor = true;
            }
            else
            {
                tensor_activeness[i].push_back(std::stoi(activeness_terms[0]));
            }
        }
        // if (!is_meta_tensor)
        // {
            Tensor* new_tensor = new Tensor(size, tensor_id, global_tensor_ids.find(tensor_id)!=global_tensor_ids.end());
            tensor_list.push_back(new_tensor);
            std::cout<<"Tensor "<<new_tensor->tensor_id<<" has been created!"<<std::endl;
            new_tensor->print();
        // }
    }

    int n_kernels;
    fin>>n_kernels;
    vector<long> kernel_times;
    kernel_times.resize(n_kernels);
    double gar;

    for (size_t i = 0; i < n_kernels; i++)
    {
        int kernel_id;
        fin >> kernel_id;
        fin >> kernel_times[i];
        fin >> gar;
        // std::cout<<"Kernel "<<i<<" exe time: "<<kernel_times[i]<<std::endl;
        iter_time += kernel_times[i];
    }

    int make_loss_index;
    
    fin >> make_loss_index;


    for (size_t i = 0; i < n_kernels; i++)
    {
        CUDAKernelType type;
        if (i==make_loss_index)
        {
            type = CUDAKernelType::makeLoss;
        }
        else
        {
            type = CUDAKernelType::Custom;
        }
        kernel_list.emplace_back(type, i, kernel_times[i]);
        // kernel_list[i].print();
        // std::cout<<kernel_list[i].execution_cycles<<std::endl;
    }

    for (size_t i = 0; i < n_tensors; i++)
    {
        for (size_t j = 0; j < tensor_activeness[i].size(); j++)
        {
            kernel_list[tensor_activeness[i][j]].outputs.insert(tensor_list[i]);
            kernel_list[tensor_activeness[i][j]].inputs.insert(tensor_list[i]);
            // std::cout<<"Tensor "<<tensor_list[i]->tensor_id<<" is needed by kernel "<<tensor_activeness[i][j]<<std::endl;
        }
    }


    for( auto it : tensor_list){
        if (it->size_in_byte == 0)
        {
            std::remove(tensor_list.begin(), tensor_list.end(), it), tensor_list.end();
            tensor_list.resize(tensor_list.size()-1);
        }
    }

    std::cout<<"Iteration time: "<<iter_time<<std::endl;


    // for (size_t i = 0; i < i_tokens.size(); i++)
    // {
    //     std::cout<<i_tokens[i]<<std::endl;
    // }
    
}

