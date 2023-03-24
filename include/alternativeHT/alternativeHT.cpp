#include "alternativeHT.h"

using namespace uvdar;

alternativeHT::alternativeHT(double decayFactor, int poly_order){
    poly_order_ = poly_order;
    extended_search_ = std::make_unique<ExtendedSearch>(decayFactor, poly_order);

}

void alternativeHT::setDebugFlags(bool debug, bool visual_debug){
    debug_ = debug;
    visual_debug_ = visual_debug;
}

void alternativeHT::setSequences(std::vector<std::vector<bool>> i_sequences){
  
    original_sequences_ = i_sequences;
    matcher_ = std::make_unique<SignalMatcher>(original_sequences_);
}

void alternativeHT::updateFramerate(double input) {
  if (input > 1.0)
    framerate_ = input;
}

void alternativeHT::processBuffer(const mrs_msgs::ImagePointsWithFloatStampedConstPtr pts_msg) {
    
    std::vector<PointState> current_frame;
    for ( auto point_time_stamp : pts_msg->points) {
        PointState p;
        p.point = cv::Point(point_time_stamp.x, point_time_stamp.y);
        p.led_state = true;
        p.insert_time = pts_msg->stamp;
        current_frame.push_back(p);
    }

    findClosestPixelAndInsert(current_frame);
    cleanPotentialBuffer();

}

void alternativeHT::findClosestPixelAndInsert(std::vector<PointState> & current_frame) {   
    
    // reference to sequences used for processing in the moving average functions
    
    std::vector<seqPointer> p_gen_seq;
    {
        // TODO: double check this 
        std::scoped_lock lock(mutex_gen_sequences_);
        p_gen_seq = gen_sequences_;
    }
    
    std::vector<PointState> no_nn;
    for(auto & curr_point : current_frame){
        std::scoped_lock lock(mutex_gen_sequences_);
        bool nn = false;
        for(auto seq = p_gen_seq.begin(); seq != p_gen_seq.end(); ++seq){
            PointState last_inserted = (*seq)->end()[-1];
            cv::Point2d diff = computeXYDiff(curr_point.point, last_inserted.point);
            if(diff.x <= max_pixel_shift_x_ && diff.y <= max_pixel_shift_y_){
                nn = true;
                insertPointToSequence(**seq, curr_point);    
                p_gen_seq.erase(seq);
                break;
            }else{
                nn = false;
            }
        }
        if(nn == false){
            no_nn.push_back(curr_point);
        }
    }
    expandedSearch(no_nn, p_gen_seq);

}

cv::Point2d alternativeHT::computeXYDiff(const cv::Point2d first, const cv::Point2d second){
    
    cv::Point2d difference; 
    difference.x = std::abs(first.x - second.x);
    difference.y = std::abs(first.y - second.y);
    return difference;
}

void alternativeHT::insertPointToSequence(std::vector<PointState> & sequence, const PointState signal){
        sequence.push_back(signal);            
        if(sequence.size() > (original_sequences_[0].size()*size_for_saved_seqs_)){
            sequence.erase(sequence.begin());
        }
}


void alternativeHT::expandedSearch(std::vector<PointState>& no_nn_current_frame, std::vector<seqPointer>& sequences_no_insert){
    
    std::scoped_lock lock(mutex_gen_sequences_);

    // std::cout << "Sequences No Insert" << sequences_no_insert.size() << " Seq " << generatedSequences_.size() << "\n";
    if((int)no_nn_current_frame.size() != 0){
        double insert_time = no_nn_current_frame[0].insert_time.toSec() + prediction_margin_;
        // std::vector<SeqWithTrajectory> sequences;
        for(int k = 0; k < (int)sequences_no_insert.size(); ++k){
            
            if(!checkSequenceValidityWithNewInsert(sequences_no_insert[k])){
                continue;
            }
            

            std::vector<double> x,y;
            std::vector<double> time;

            // prepare for polynomial regression TODO: Maybe calculate the variance over the points for outlier rejection
            for(const auto point : *sequences_no_insert[k]){
                if(point.led_state){
                    x.push_back(point.point.x);
                    y.push_back(point.point.y);
                    time.push_back(point.insert_time.toSec());
                }
            }

            PointState& last_point = sequences_no_insert[k]->end()[-1]; 
            
            PredictionStatistics x_predictions = selectStatisticsValues(x, time, insert_time, max_pixel_shift_x_);
            PredictionStatistics y_predictions = selectStatisticsValues(y, time, insert_time, max_pixel_shift_y_);
            last_point.ellipse.x = x_predictions.ellipse_val;
            last_point.predicted.x = x_predictions.predicted_coordinate;
            last_point.x_coeff = x_predictions.coeff;

            last_point.ellipse.y = y_predictions.ellipse_val;
            last_point.predicted.y= y_predictions.predicted_coordinate;
            last_point.y_coeff = y_predictions.coeff;
            last_point.extended_search = true;

            for(int i = 0; i < (int)no_nn_current_frame.size(); i++){
                if(extended_search_->checkIfInsideEllipse(last_point.point, last_point.ellipse, no_nn_current_frame[i].point)){
                    no_nn_current_frame[i].extended_search = true;
                    no_nn_current_frame[i].x_coeff = last_point.x_coeff;
                    no_nn_current_frame[i].y_coeff = last_point.y_coeff;
                    no_nn_current_frame[i].ellipse = last_point.ellipse;
                    no_nn_current_frame[i].predicted = last_point.predicted;
                    insertPointToSequence(*sequences_no_insert[k], no_nn_current_frame[i]);
                    no_nn_current_frame.erase(no_nn_current_frame.begin()+i);
                    sequences_no_insert.erase(sequences_no_insert.begin()+k); // TODO:throws error at certain point!
                    break;
                }   
            }
        }
    }

    // maybe use bigger search space when still points with no association exist
    if(sequences_no_insert.size() != 0 && no_nn_current_frame.size() != 0){
        // for(auto seq : sequences_no_insert){
        //     if(!seq->end()[-1].extended_search){
        //         continue;
        //     }

        //     for(auto point : no_nn_current_frame){

        //     }
        // }
    }

    for(auto seq : sequences_no_insert){
        insertVPforSequencesWithNoInsert(seq);
    }

    // if still points are not inserted start new sequence
    for(auto point : no_nn_current_frame){
        std::vector<PointState> vect;
        vect.push_back(point);
        gen_sequences_.push_back(std::make_shared<std::vector<PointState>>(vect));
    }
}

bool alternativeHT::checkSequenceValidityWithNewInsert(const seqPointer & seq){

    if(seq->size() > 1){
        // if the last two led states were on - no "on" point expected -> no NN search necessary
       if(seq->end()[-1].led_state == true && seq->end()[-2].led_state == true){
            return false;
        }
    }

    if(seq->size() > 2){
        // if last three led states are off, the sequence is illegal anyways 
        if(seq->end()[-1].led_state == false && seq->end()[-2].led_state == false && seq->end()[-3].led_state == false){
            return false;
        }
    }
    return true; 
}

void alternativeHT::insertVPforSequencesWithNoInsert(seqPointer & seq){
    PointState pVirtual;
    pVirtual = seq->end()[-1];
    pVirtual.insert_time = ros::Time::now();
    pVirtual.led_state = false;
    // pVirtual.extended_search = false;
    insertPointToSequence(*seq, pVirtual);
}

PredictionStatistics alternativeHT::selectStatisticsValues(const std::vector<double>& values, const std::vector<double>& time, const double& insert_time, const int& max_pix_shift){

    auto weight_vect = extended_search_->calculateWeightVector(time);
    auto weighted_mean = extended_search_->calcWeightedMean(values, weight_vect); 
    auto std = extended_search_->calcWSTD(values, weight_vect, weighted_mean);

    PredictionStatistics statistics;
    statistics.mean = weighted_mean;
           
    bool all_coeff_zero = false, poly_reg_computed = false;

    // if(std > max_pix_shift){

        statistics = extended_search_->polyReg(values, time, weight_vect);
        auto coeff = statistics.coeff;
        all_coeff_zero = std::all_of(coeff.begin(), coeff.end(), [](double coeff){return coeff == 0;});
        if(!all_coeff_zero){
            for(int i = 0; i < (int)coeff.size(); ++i){
                statistics.predicted_coordinate += coeff[i]*pow(insert_time, i);
            }
            poly_reg_computed = true;
        }
        if((int)values.size() > statistics.used_poly_order){
            statistics.ellipse_val = extended_search_->confidenceInterval(statistics, values, weight_vect, 75);
            // std::cout << statistics.ellipse_val <<"HERE\n"; 
        }else{
            // std::cout << "VALU sioze " << values.size() << "\n";
            poly_reg_computed = false;
        }
    // }
    
    if(!poly_reg_computed){
        // std::cout << "NO POLy\n";
        statistics.predicted_coordinate = weighted_mean; 
        statistics.ellipse_val = (std < max_pix_shift) ? max_pix_shift*2 : std*2; // TODO: BULLSHIT i guess
        statistics.coeff = std::vector<double>(1, 0.0);
    }

    return statistics;
            
}

void alternativeHT::cleanPotentialBuffer(){

    std::scoped_lock lock(mutex_gen_sequences_);
    double timing_tolerance = 5/60;
    if(framerate_ != 0){
        timing_tolerance = (1/framerate_) * 4; 
    }

    // std::cout << "Time margin " << timeMargin; 

    auto it = gen_sequences_.begin();
    while(it != gen_sequences_.end()){ 
        if ((*it)->empty()){
            ++it;
            continue;
        }

        if( (*it)->size() > 2){
            if(!(*it)->end()[-1].led_state && !(*it)->end()[-2].led_state && !(*it)->end()[-3].led_state){
                it = gen_sequences_.erase(it);
                continue;
            }
        }

        // works but eventually not usefull in that current implementation
        double insert_time = (*it)->end()[-1].insert_time.toSec(); 
        double diff_last_insert = std::abs(insert_time - ros::Time::now().toSec());
        if (diff_last_insert > timing_tolerance){ 
            it = gen_sequences_.erase(it);
            continue;
        }
        ++it;

    }
}

std::vector<std::pair<std::vector<PointState>, int>> alternativeHT::getResults(){

    std::scoped_lock lock(mutex_gen_sequences_);
    std::vector<std::pair<std::vector<PointState>, int>> retrievedSignals;

    for (auto sequence : gen_sequences_){
        std::vector<bool> led_states;
        std::vector<PointState> return_seq; 
        for(auto p : *sequence){
            return_seq.push_back(p);
        }

        std::vector<PointState> selected;
        if ((int)return_seq.size() > (int)original_sequences_[0].size()){
            int diff = (int)return_seq.size() - (int)original_sequences_[0].size();
            // std::cout << "the diff" << diff << "\n";

            for(int i = diff; i < (int)return_seq.size(); ++i){
                selected.push_back(return_seq[i]);
            }
        }else{

            selected = return_seq;
        }
        // std::cout << "size " << selected.size() << "\n";
        for (auto point : selected){
            led_states.push_back(point.led_state);
        }

        int id = findSequenceMatch(led_states);



        // retrievedSignals.push_back(std::make_pair(sequence.end()[-1], id));
        retrievedSignals.push_back(std::make_pair(return_seq, id));
        // if(sequence.end()[-1].computed_extended_search)std::cout << "Predicted " << sequence.end()[-1].predicted.x << " " << sequence.end()[-1].predicted.y << std::endl; 
       
    }

    return retrievedSignals;
}

int alternativeHT::findSequenceMatch(std::vector<bool> sequence){

    for ( int i = 2; i < (int)sequence.size(); i++) {
        if (sequence[i] == false && sequence[i-1] == false && sequence[i-2] == false){
            return -2;
        }
    }
    int id = matcher_->matchSignalWithCrossCorr(sequence);

    return id;
}

alternativeHT::~alternativeHT() {
    // delete generatedSequences_;
}