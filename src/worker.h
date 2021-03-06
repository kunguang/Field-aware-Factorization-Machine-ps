#include <algorithm>
#include <iostream>

#include <mutex>
#include <functional>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <pmmintrin.h>
#include <immintrin.h>
#include "sparsehash_memory/sparsepp.h"

#include "sparsehash_cpu/internal/sparseconfig.h"
#include "sparsehash_cpu/type_traits.h"
#include "sparsehash_cpu/sparsetable"
#include "hash_test_interface.h"

#include "./io/load_data.cc"
#include "threadpool/thread_pool.h"
#include "ps.h"

#include <netdb.h>  
#include <net/if.h>  
#include <arpa/inet.h>  
#include <sys/ioctl.h>  
#include <sys/types.h>  
#include <sys/socket.h>  

#define IP_SIZE     16

namespace dmlc{
class Worker : public ps::App{
    public:
        Worker(const char *train_file, const char *test_file) : 
                train_file_path(train_file), test_file_path(test_file){ 
            char ip[IP_SIZE];
            const char *test_eth = "eth0"; 
            get_local_ip(test_eth, ip);
            std::cout<<"worker ip "<<ip<<std::endl;
        }
        ~Worker(){
            delete train_data;
            //delete test_data;
        } 
 
    void get_local_ip(const char *eth_inf, char *ip)  {  
        int sd;  
        struct sockaddr_in sin;  
        struct ifreq ifr;  
        sd = socket(AF_INET, SOCK_DGRAM, 0);  
        if (-1 == sd){  
            std::cout<<"socket error: "<<strerror(errno)<<std::endl;  
            return;        
        }  
        strncpy(ifr.ifr_name, eth_inf, IFNAMSIZ);  
        ifr.ifr_name[IFNAMSIZ - 1] = 0;  
        if (ioctl(sd, SIOCGIFADDR, &ifr) < 0){  
            std::cout<<"ioctl error: "<<strerror(errno)<<std::endl;  
            close(sd);  
            return;  
        }  
        memcpy(&sin, &ifr.ifr_addr, sizeof(sin));  
        snprintf(ip, IP_SIZE, "%s", inet_ntoa(sin.sin_addr));  
        close(sd);  
    }  

        virtual void ProcessRequest(ps::Message* request){
            //do nothing.
        }

        float sigmoid(float x){
            if(x < -30) return 1e-6;
            else if(x > 30) return 1.0;
            else{
                double ex = pow(2.718281828, x);
                return ex / (1.0 + ex);
            }
        }

	    virtual bool Run(){
	        Process();
	    }

        inline void filter_zero_element(std::vector<float>& gradient, std::vector<ps::Key>& nonzero_index, std::vector<float>& nonzero_gradient){
            for(int i = 0; i < init_index.size(); i++){
                int idx = init_index[i];
                float g = gradient[idx];
                if(g != 0.0){
                    nonzero_index.push_back(idx);
                    nonzero_gradient.push_back(g);
                }
            }
        }

        timespec time_diff(timespec start, timespec end){
            timespec tmp;
            tmp.tv_sec =  end.tv_sec - start.tv_sec;
            tmp.tv_nsec = end.tv_nsec - start.tv_nsec;
            return tmp;
        }

        struct sample_key{
            size_t fid;
            int sid;
        };

        static bool sort_finder(const sample_key& a, const sample_key& b){
            return a.fid < b.fid;
        }

        static bool unique_finder(const sample_key& a, const sample_key& b){
            return a.fid == b.fid;
        }

        void calculate_batch_gradient(int start, int end){
            timespec all_start, all_end, all_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &all_start);
            size_t idx = 0; int value = 0; float pctr = 0;
            auto all_keys = std::vector<sample_key>();
            auto unique_keys = std::make_shared<std::vector<ps::Key>> ();;
            int line_num = 0;
            for(int row = start; row < end; ++row){
                int sample_size = train_data->fea_matrix[row].size();
                sample_key sk;
                sk.sid = line_num;
                for(int j = 0; j < sample_size; ++j){//for one instance
                    idx = h(train_data->fea_matrix[row][j].fid);
                    sk.fid = idx;
                    all_keys.push_back(sk);
                    (*unique_keys).push_back(idx);
                }
                ++line_num;
            }
            std::sort(all_keys.begin(), all_keys.end(), Worker::sort_finder);
            std::sort((*unique_keys).begin(), (*unique_keys).end());
            (*unique_keys).erase(unique((*unique_keys).begin(), (*unique_keys).end()), (*unique_keys).end());
            int keys_size = (*unique_keys).size();

            auto w = std::make_shared<std::vector<float>>();
            timespec pull_start_time, pull_end_time, pull_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &pull_start_time);

            kv_.Wait(kv_.ZPull(unique_keys, &(*w)));

            clock_gettime(CLOCK_MONOTONIC, &pull_end_time);
            pull_elapsed_time = time_diff(pull_start_time, pull_end_time);
            
            auto wx = std::vector<float>(end - start + 1);
            for(int j = 0, i = 0; j < all_keys.size();){
                int allkeys_fid = all_keys[j].fid;
                int weight_fid = (*unique_keys)[i];
                if(allkeys_fid == weight_fid){
                    wx[all_keys[j].sid] += (*w)[i];
                    ++j;
                }
                else if(allkeys_fid > weight_fid){ 
                    ++i;
                }
            }
            
            for(int i = 0; i < wx.size(); i++){
                pctr = sigmoid(wx[i]);
                float loss = pctr - train_data->label[start++];
                wx[i] = loss;
            }

            auto push_gradient = std::make_shared<std::vector<float> > (keys_size);
            for(int j = 0, i = 0; j < all_keys.size();){
                int allkeys_fid = all_keys[j].fid;
                int gradient_fid = (*unique_keys)[i];
                int sid = all_keys[j].sid;
                if(allkeys_fid == gradient_fid){
                    (*push_gradient)[i] += wx[sid];
                    ++j;
                }
                else if(allkeys_fid > gradient_fid){
                    ++i;
                }
            }

            timespec push_start_time, push_end_time, push_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &push_start_time);
            kv_.Wait(kv_.ZPush(unique_keys, push_gradient));//put gradient to servers;
            clock_gettime(CLOCK_MONOTONIC, &push_end_time);
            push_elapsed_time = time_diff(push_start_time, push_end_time);
            clock_gettime(CLOCK_MONOTONIC, &all_end);
            all_elapsed_time = time_diff(all_start, all_end);

            all_time += all_elapsed_time.tv_sec * 1e9 + all_elapsed_time.tv_nsec;
            all_pull_time += pull_elapsed_time.tv_sec * 1e9 + pull_elapsed_time.tv_nsec;
            all_push_time += push_elapsed_time.tv_sec * 1e9 + push_elapsed_time.tv_nsec;
            send_key_numbers += keys_size;
        }

        void calculate_batch_gradient_for_netIO(int start, int end){
            auto keys = std::make_shared<std::vector<size_t>> ();
            std::cout<<" size_t size "<<sizeof(size_t)<<std::endl;
            for(int row = start; row < end; ++row){
                int sample_size = train_data->fea_matrix[row].size();
                for(int j = 0; j < sample_size; ++j){//for one instance
                    size_t idx = train_data->fea_matrix[row][j].fid;
                    (*keys).push_back(idx);
                }
            }
            std::sort((*keys).begin(), (*keys).end());
            std::vector<ps::Key>::iterator iter_keys;
            iter_keys = unique((*keys).begin(), (*keys).end());
            (*keys).erase(iter_keys, (*keys).end());
            //std::cout<<"thread batch keys size "<<(*keys).size()<<std::endl;

            auto initdata = std::make_shared<std::vector<float>>();
            for(int i = 0; i < (*keys).size(); i++) (*initdata).push_back(0.01);
            kv_.Wait(kv_.ZPush(keys, initdata));

            auto w = std::make_shared<std::vector<float>>();
            while(1){
                //std::cout<<" pthread_id "<<pthread_self()<<1<<std::endl;
                kv_.Wait(kv_.ZPull(keys, &(*w)));
                //std::cout<<" pthread_id" <<pthread_self()<<2<<std::endl;
                kv_.Wait(kv_.ZPush(keys, w));//put gradient to servers;
                //std::cout<<" pthread_id "<<pthread_self()<<3<<std::endl;
            }
        }

        void batch_learning_for_netIO(int core_num){
            train_data = new dml::LoadData(train_data_path);
            std::hash<size_t> h;
            int train_data_size = 100000;
            train_data->fea_matrix.clear();
            std::srand(std::time(0));
            std::vector<size_t> k;
            for(int i = 0; i < train_data_size; i++){
                if(i % 20000 == 0) std::cout<<" init train_data "<<i<<std::endl;
                train_data->sample.clear();
                for(int j = 0; j < 20000; j++){
                    size_t idx = h(std::rand());                    
                    train_data->keyval.fid = idx;
                    k.push_back(train_data->keyval.fid);
                    train_data->sample.push_back(train_data->keyval);
                }
                train_data->fea_matrix.push_back(train_data->sample);
            }
            std::cout<<"train_data size : "<<train_data->fea_matrix.size()<<std::endl;

            ThreadPool pool(core_num);

            batch_num = train_data->fea_matrix.size() / batch_size;
            std::cout<<"batch_num : "<<batch_num<<" core_num "<<core_num<<std::endl;

            int epoch = 0;
            while(epoch < 40){
                for(int i = 0; i < batch_num; ++i){
                    int all_start = i * batch_size;
                    int thread_batch = batch_size / core_num;
                    for(int j = 0; j < core_num; ++j){
                        int start = all_start + j * thread_batch;
                        int end = all_start + (j + 1) * thread_batch;
                        pool.enqueue(std::bind(&Worker::calculate_batch_gradient_for_netIO, this, start, end));
                    }
                }//end all batch
                std::cout<<"epoch "<<epoch<<std::endl;
                epoch++;
            }//end all epoch
        }//end batch_learning

        void batch_learning(int core_num){
            train_data = new dml::LoadData(train_data_path);
            train_data->load_all_data();
            //std::hash<std::string> h;
            ThreadPool pool(core_num);

            batch_num = train_data->fea_matrix.size() / batch_size;
            std::cout<<"batch_num : "<<batch_num<<std::endl;
            timespec allstart, allend, allelapsed;
            for(int epoch = 0; epoch < epochs; ++epoch){
                size_t old_all_time = 0;
                size_t old_all_push_time = 0;
                size_t old_all_pull_time = 0;
                clock_gettime(CLOCK_MONOTONIC, &allstart);
                send_key_numbers = 0;
                for(int i = 0; i < batch_num; ++i){
                    int all_start = i * batch_size;
                    int all_end = (i + 1)* batch_size;
                    int thread_batch = batch_size / core_num;
                    for(int j = 0; j < core_num; ++j){
                        int start = all_start + j * thread_batch;
                        int end = all_start + (j + 1) * thread_batch;
                        pool.enqueue(std::bind(&Worker::calculate_batch_gradient, this, start, end));
                    }
                }//end all batch
                clock_gettime(CLOCK_MONOTONIC, &allend);
                allelapsed = time_diff(allstart, allend);
                std::cout<<"rank "<<rank<<" per process : "<<train_data->fea_matrix.size() * 1e9 * 1.0 / (allelapsed.tv_sec * 1e9 + allelapsed.tv_nsec)<<std::endl;
                std::cout<<"rank "<<rank<<" send_key_number avage: "<<send_key_numbers * 1.0 / (batch_num * core_num)<<std::endl;
            }//end all epoch
        }//end batch_learning

        virtual void Process(){
            rank = ps::MyRank();
            snprintf(train_data_path, 1024, "%s-%05d", train_file_path, rank);
            core_num = std::thread::hardware_concurrency();
            //batch_learning(core_num);
            batch_learning_for_netIO(core_num);
            std::cout<<"train end......"<<std::endl;
        }//end process

    public:
        int rank;
        int core_num;
        int batch_num;
        int call_back = 1;
        int batch_size = 3200;
        int epochs = 10;
        int calculate_gradient_thread_count;
        int is_online_learning = 0;
        int is_batch_learning = 1;

        std::atomic_llong  num_batch_fly = {0};
        std::atomic_llong all_time = {0};
        std::atomic_llong all_push_time = {0};
        std::atomic_llong all_pull_time = {0};
        std::atomic_llong send_key_numbers = {0};
        std::hash<size_t> h;
        
        std::mutex mutex;
        std::vector<ps::Key> init_index;
        dml::LoadData *train_data;
        dml::LoadData *test_data;
        const char *train_file_path;
        const char *test_file_path;
        char train_data_path[1024];
        char test_data_path[1024];
        float bias = 0.0;
        ps::KVWorker<float> kv_;
};//end class worker

}//end namespace dmlc 
