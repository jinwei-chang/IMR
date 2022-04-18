#include "IMR_Crosstrack.h"

void IMR_Crosstrack::initialize(std::ifstream &setting_file){
    // * init options
    IMR_Base::initialize(setting_file);

    // * init 
	track_written.resize(options.TOTAL_TRACKS, false);

    // * init write position
    write_position = 0;
}

void IMR_Crosstrack::run(std::ifstream &input_file, std::ofstream &output_file){
    read_file(input_file);
    size_t processing = 0;

    while(!order_queue.empty()){
        Request trace = order_queue.top();
        order_queue.pop();
        trace.address -= eval.shifting_address;

        if(processing % (eval.trace_total_requests / 100) == 0){
            std::clog << "<log> processing " << processing << "\r" << std::flush;
        }

        if(processing != 0 && processing % (eval.trace_total_requests / options.APPEND_PARTS) == 0){
            size_t append_size = eval.append_trace_size * options.APPEND_COLD_SIZE;
            size_t remainder = options.TOTAL_SECTORS - eval.total_sector_used;
            if(append_size > remainder){
                append_size = remainder / options.APPEND_PARTS;
            }
            std::clog << "<log> append trace " << append_size << " at processing " << processing << std::endl;

            for(size_t append = append_size; append > 0 && append > (1 << 20); append -= 1 << 20){
                Request append_trace(
                    trace.timestamp,
                    'W',
                    eval.max_LBA + 1,
                    1 << 20,
                    trace.device
                );
                write_append(append_trace, output_file);
                eval.total_sector_used += 1 << 20;
            }

            eval.append_count += 1;
        }

        // * read request
        if(trace.iotype == 'R' || trace.iotype == '1'){
            trace.iotype = 'R';
            read(trace, output_file);
        }
        // * write request
        else if(trace.iotype == 'W' || trace.iotype == '0'){
            trace.iotype = 'W';
            write(trace, output_file);
        }

        processing++;
    }
}

void IMR_Crosstrack::write(const Request &request, std::ostream &output_file){
    if(options.UPDATE_METHOD == Update_Method::IN_PLACE){
        inplace_crosstrack_write(request, output_file);
    }
    else if(options.UPDATE_METHOD == Update_Method::OUT_PLACE){
        // std::clog << options.UPDATE_METHOD << std::endl;
        outplace_crosstrack_write(request, output_file);
    }
}

void IMR_Crosstrack::write_append(const Request &request, std::ostream &output_file){
    std::vector<Request> requests;
    for(size_t i = 0; i < request.size; ++i){
        size_t LBA = request.address;
        // size_t PBA = get_PBA(LBA);

        size_t current_write_track = get_track(write_position);
        Request writeRequest(
            request.timestamp,
            'W',
            write_position,
            1,
            request.device
        );

        requests.push_back(writeRequest);
        set_LBA_to_PBA(LBA, write_position);
        track_written[current_write_track] = true;
        
        if (current_write_track != get_track(write_position + 1)) {
            current_write_track += 2;
            write_position = get_track_head_sector(current_write_track);
            
            if (!isTop(current_write_track) && current_write_track >= options.TOTAL_TRACKS) {
                // * move to first TOP track
                write_position = get_track_head_sector(1);
            }
        }
        else
            write_position += 1;
    }

    write_requests_file(requests, output_file);
}

void IMR_Crosstrack::inplace_crosstrack_write(const Request &request, std::ostream &output_file){
    std::vector<Request> requests;

    size_t previous_PBA = -1;

    size_t update_length = 0;

    for(size_t i = 0; i < request.size; ++i){
        size_t LBA = request.address + i;
        size_t PBA = get_PBA(LBA);

        if(PBA == -1){
            size_t current_write_track = get_track(write_position);
            Request writeRequest(
                request.timestamp,
                'W',
                write_position,
                1,
                request.device
            );

            requests.push_back(writeRequest);
            set_LBA_to_PBA(LBA, write_position);
            track_written[current_write_track] = true;
            
            if (current_write_track != get_track(write_position + 1)) {
				current_write_track += 2;
				write_position = get_track_head_sector(current_write_track);
                
				if (!isTop(current_write_track) && current_write_track >= options.TOTAL_TRACKS) {
                    // * move to first TOP track
					write_position = get_track_head_sector(1);
				}
			}
			else
				write_position += 1;
        }
        else{
            update_length += 1;
            eval.update_times += 1;

            size_t current_update_track = get_track(PBA);

            if(
                isTop(current_update_track) 
            ){
                if(isTop(current_update_track) )
                    eval.direct_update_top_count += 1;
                else 
                    eval.direct_update_bottom_count += 1;

                Request writeRequest(
                    request.timestamp,
                    'W',
                    PBA,
                    1,
                    request.device
                );

                requests.push_back(writeRequest);
            }
            else {
                eval.inplace_update_count += 1;

                size_t previous_update_track = get_track(previous_PBA);

                if(current_update_track != previous_update_track){
                    // * read left top track
                    if(current_update_track >= 1 && track_written[current_update_track - 1]) {
                        Request readRequest(
                            request.timestamp,
                            'R',
                            get_track_head_sector(current_update_track - 1),
                            options.SECTORS_PER_TOP_TRACK,
                            request.device
                        );

                        requests.push_back(readRequest);
                    }
                    // * read right top track
                    if(current_update_track < options.TOTAL_TRACKS && track_written[current_update_track + 1]) {
                        Request readRequest(
                            request.timestamp,
                            'R',
                            get_track_head_sector(current_update_track + 1),
                            options.SECTORS_PER_TOP_TRACK,
                            request.device
                        );

                        requests.push_back(readRequest);
                    }
                }

                Request writeRequest(
                    request.timestamp,
                    'W',
                    PBA,
                    1,
                    request.device
                );

                requests.push_back(writeRequest);

                if(
                    i == request.size - 1 ||
                    current_update_track != get_track(get_PBA(LBA + 1))
                ){
                    if(current_update_track >= 1 && track_written[current_update_track - 1]) {
                        Request writeBackLeftTopRequest(
                            request.timestamp,
                            'W',
                            get_track_head_sector(current_update_track - 1),
                            options.SECTORS_PER_TOP_TRACK,
                            request.device
                        );
                        requests.push_back(writeBackLeftTopRequest);
                    }
                    
                    if(current_update_track < options.TOTAL_TRACKS && track_written[current_update_track + 1]) {
                        Request writeBackRightTopRequest(
                            request.timestamp,
                            'W',
                            get_track_head_sector(current_update_track + 1),
                            options.SECTORS_PER_TOP_TRACK,
                            request.device
                        );
                        requests.push_back(writeBackRightTopRequest);
                    }
                }

            }
        }

        previous_PBA = PBA;
    }

    eval.insert_update_dist(update_length);
	
    // * output
    write_requests_file(requests, output_file);
}

void IMR_Crosstrack::outplace_crosstrack_write(const Request &request, std::ostream &output_file){
    std::vector<Request> requests;

    size_t update_length = 0;

    for(size_t i = 0; i < request.size; ++i){
        size_t LBA = request.address + i;
        size_t PBA = get_PBA(LBA);

        if(PBA == -1){
            size_t current_write_track = get_track(write_position);

            Request writeRequest(
                request.timestamp,
                'W',
                write_position,
                1,
                request.device
            );

            requests.push_back(writeRequest);
            set_LBA_to_PBA(LBA, write_position);
            track_written[current_write_track] = true;
            
            if (current_write_track != get_track(write_position + 1)) {
                current_write_track += 2;
				write_position = get_track_head_sector(current_write_track);
                
				if (!isTop(current_write_track) && current_write_track >= options.TOTAL_TRACKS) {
                    // * move to first TOP track
					write_position = get_track_head_sector(1);
				}
			}
			else
				write_position += 1;
        }
        // * PBA exists, update
        else{
            update_length += 1;
            eval.update_times += 1;

            size_t current_update_track = get_track(PBA);

            if(
                isTop(current_update_track) 
                || (current_update_track == 0 && !track_written[current_update_track + 1])
                || (!track_written[current_update_track - 1] && !track_written[current_update_track + 1])
            ){
                if(isTop(current_update_track) )
                    eval.direct_update_top_count += 1;
                else 
                    eval.direct_update_bottom_count += 1;

                Request writeRequest(
                    request.timestamp,
                    'W',
                    PBA,
                    1,
                    request.device
                );

                requests.push_back(writeRequest);
                set_LBA_to_PBA(LBA, PBA);
                track_written[current_update_track] = true;
            }
            else{
                eval.outplace_update_count += 1;

                size_t current_write_track = get_track(write_position);
                Request writeRequest(
                    request.timestamp,
                    'W',
                    write_position,
                    1,
                    request.device
                );

                requests.push_back(writeRequest);
                set_LBA_to_PBA(LBA, write_position);
                track_written[current_write_track] = true;
                
                if (current_write_track != get_track(write_position + 1)) {
                    current_write_track += 2;
                    write_position = get_track_head_sector(current_write_track);
                    
                    if (!isTop(current_write_track) && current_write_track >= options.TOTAL_TRACKS) {
                        // * move to first TOP track
                        write_position = get_track_head_sector(1);
                    }
                }
                else
                    write_position += 1;
            }
        }
    }

    eval.insert_update_dist(update_length);

    // * output
    write_requests_file(requests, output_file);
}

void IMR_Crosstrack::evaluation(std::string &evaluation_file){
    IMR_Base::evaluation(evaluation_file);

    evaluation_stream << "=== Crosstrack Evaluation ==="                                 << "\n";
    evaluation_stream << "Last Write Position: "                    << write_position << "\n";
    evaluation_stream << "Direct Update Bottom Count (sector): "    << eval.direct_update_bottom_count << "\n";
    evaluation_stream << "Direct Update Top Count (sector): "       << eval.direct_update_top_count << "\n";
    evaluation_stream << "Inplace Update Count (sector): "          << eval.inplace_update_count << "\n";
    evaluation_stream << "Outplace Update Count (sector): "         << eval.outplace_update_count << "\n";
}