//
// Copyright 2015 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "octoclock_impl.hpp"
#include "common.h"
#include "kk_ihex_read.h"

#include <uhd/device.hpp>
#include <uhd/image_loader.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/time_spec.hpp>
#include <uhd/utils/byteswap.hpp>
#include <uhd/utils/paths.hpp>
#include <uhd/utils/static.hpp>

#include <boost/cstdint.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = boost::filesystem;
using namespace uhd;
using namespace uhd::usrp_clock;
using namespace uhd::transport;

#define OCTOCLOCK_FIRMWARE_MAX_SIZE_BYTES (1024*120) // Last 8 MB are for bootloader
#define OCTOCLOCK_BLOCK_SIZE              256

/*
 * OctoClock burn session
 */
typedef struct {
    bool                        found;
    uhd::device_addr_t          dev_addr;
    std::string                 given_filepath;
    std::string                 actual_filepath; // If using a .hex, this is the converted .bin
    bool                        from_hex;
    boost::uint32_t             size;
    boost::uint16_t             crc;
    boost::uint16_t             num_blocks;
    udp_simple::sptr            ctrl_xport;
    udp_simple::sptr            fw_xport;
    boost::uint8_t              data_in[udp_simple::mtu];
} octoclock_session_t;

static void octoclock_calculate_crc(octoclock_session_t &session){
    std::ifstream ifile(session.actual_filepath.c_str());
    boost::uint8_t temp_image[OCTOCLOCK_FIRMWARE_MAX_SIZE_BYTES];
    ifile.read((char*)temp_image, session.size);

    session.crc = 0xFFFF;
    for(size_t i = 0; i < session.size; i++){
        session.crc ^= temp_image[i];
        for(boost::uint8_t j = 0; j < 8; ++j){
            if(session.crc & 1) session.crc = (session.crc >> 1) ^ 0xA001;
            else session.crc = (session.crc >> 1);
        }
    }

    ifile.close();
}

static void octoclock_convert_ihex(octoclock_session_t &session){
    struct ihex_state ihex;
    ihex_count_t count;
    char buf[256];
    FILE* infile = fopen(session.given_filepath.c_str(), "r");
    FILE* outfile = fopen(session.actual_filepath.c_str(), "w");
    uint64_t line_number = 1;

    ihex_begin_read(&ihex);
    while(fgets(buf, 256, infile)){
        count = ihex_count_t(strlen(buf));
        ihex_read_bytes(&ihex, buf, count, outfile);
        line_number += (count && buf[count - 1] == '\n');
    }
    ihex_end_read(&ihex, outfile); // Closes outfile

    (void)fclose(infile);
}

static void octoclock_validate_firmware_image(octoclock_session_t &session){
    if(not fs::exists(session.given_filepath)){
        throw uhd::runtime_error(str(boost::format("Could not find image at path \"%s\"")
                                     % session.given_filepath));
    }

    std::string extension = fs::extension(session.given_filepath);
    if(extension == ".bin"){
        session.actual_filepath = session.given_filepath;
        session.from_hex = false;
    }
    else if(extension == ".hex"){
        session.actual_filepath = fs::path(fs::path(uhd::get_tmp_path()) /
                                           str(boost::format("octoclock_fw_%d.bin")
                                               % time_spec_t::get_system_time().get_full_secs())
                                          ).string();

        octoclock_convert_ihex(session);
        session.from_hex = true;
    }
    else throw uhd::runtime_error(str(boost::format("Invalid extension \"%s\". Extension must be .hex or .bin.")));

    session.size = fs::file_size(session.actual_filepath);
    if(session.size > OCTOCLOCK_FIRMWARE_MAX_SIZE_BYTES){
        throw uhd::runtime_error(str(boost::format("The specified firmware image is too large: %d vs. %d")
                                     % session.size % OCTOCLOCK_FIRMWARE_MAX_SIZE_BYTES));
    }

    session.num_blocks = (session.size % OCTOCLOCK_BLOCK_SIZE) ? ((session.size / OCTOCLOCK_BLOCK_SIZE) + 1)
                                                               : (session.size / OCTOCLOCK_BLOCK_SIZE);

    octoclock_calculate_crc(session);
}

static void octoclock_setup_session(octoclock_session_t &session,
                                    const uhd::device_addr_t &args,
                                    const std::string &filepath){

    // See if we can find an OctoClock with the given args
    device_addrs_t devs = octoclock_find(args);
    if(devs.size() == 0){
        session.found = false;
        return;
    }
    else if(devs.size() > 1){
        std::string err_msg = "Could not resolve given args to a single OctoClock device.\n"
                              "Applicable devices:\n";

        BOOST_FOREACH(const uhd::device_addr_t &dev, devs){
            std::string name = (dev["type"] == "octoclock") ? str(boost::format("OctoClock r%d")
                                                                  % dev.get("revision","4"))
                                                          : "OctoClock Bootloader";
            err_msg += str(boost::format(" * %s (addr=%s)\n")
                           % name
                           % dev.get("addr"));
        }

        err_msg += "\nSpecify one of these devices with the given args to load an image onto it.";

        throw uhd::runtime_error(err_msg);
    }

    session.dev_addr = devs[0];

    // If no filepath is given, use the default
    if(filepath == ""){
        session.given_filepath = find_image_path(str(boost::format("octoclock_r%s_fw.hex")
                                                     % session.dev_addr.get("revision","4")
                                                 ));
    }
    else session.given_filepath = filepath;

    octoclock_validate_firmware_image(session);

    session.ctrl_xport = udp_simple::make_connected(session.dev_addr["addr"],
                                                    BOOST_STRINGIZE(OCTOCLOCK_UDP_CTRL_PORT));
    session.fw_xport   = udp_simple::make_connected(session.dev_addr["addr"],
                                                    BOOST_STRINGIZE(OCTOCLOCK_UDP_FW_PORT));
}

static void octoclock_reset_into_bootloader(octoclock_session_t &session){

    // Already in bootloader
    if(session.dev_addr["type"] == "octoclock-bootloader")
        return;

    octoclock_packet_t pkt_out;
    pkt_out.sequence = uhd::htonx<boost::uint32_t>(std::rand());
    const octoclock_packet_t* pkt_in = reinterpret_cast<const octoclock_packet_t*>(session.data_in);
    size_t len;

    std::cout << " -- Resetting into bootloader..." << std::flush;
    UHD_OCTOCLOCK_SEND_AND_RECV(session.ctrl_xport, RESET_CMD, pkt_out, len, session.data_in);
    if(UHD_OCTOCLOCK_PACKET_MATCHES(RESET_ACK, pkt_out, pkt_in, len)){

        // Make sure this device is now in its bootloader
        boost::this_thread::sleep(boost::posix_time::milliseconds(500));
        uhd::device_addrs_t octoclocks = uhd::device::find(
                                             uhd::device_addr_t(str(boost::format("addr=%s")
                                                                    % session.dev_addr["addr"]
                                                               )));
        if(octoclocks.size() == 0){
            std::cout << "failed." << std::endl;
            throw uhd::runtime_error("Failed to reset OctoClock.");
        }
        else if(octoclocks[0]["type"] != "octoclock-bootloader"){
            std::cout << "failed." << std::endl;
            throw uhd::runtime_error("Failed to reset OctoClock.");
        }
        else{
            std::cout << "successful." << std::endl;
            session.dev_addr = octoclocks[0];
        }
    }
    else{
        std::cout << "failed." << std::endl;
        throw uhd::runtime_error("Failed to reset OctoClock.");
    }
}

static void octoclock_burn(octoclock_session_t &session){

    // Make sure we're in the bootloader for this
    octoclock_reset_into_bootloader(session);

    octoclock_packet_t pkt_out;
    pkt_out.sequence = htonx<boost::uint32_t>(std::rand());
    const octoclock_packet_t* pkt_in = reinterpret_cast<const octoclock_packet_t*>(session.data_in);

    // Tell OctoClock to prepare for burn
    pkt_out.len = htonx<boost::uint16_t>(session.size);
    size_t len = 0;
    std::cout << " -- Preparing OctoClock for firmware load..." << std::flush;
    pkt_out.len = session.size;
    pkt_out.crc = session.crc;
    UHD_OCTOCLOCK_SEND_AND_RECV(session.fw_xport, PREPARE_FW_BURN_CMD, pkt_out, len, session.data_in);
    if(UHD_OCTOCLOCK_PACKET_MATCHES(FW_BURN_READY_ACK, pkt_out, pkt_in, len)){
        std::cout << "successful." << std::endl;
    }
    else{
        std::cout << "failed." << std::endl;
        if(session.from_hex){
            fs::remove(session.actual_filepath);
        }
        throw uhd::runtime_error("Failed to prepare OctoClock for firmware load.");
    }

    // Start burning
    std::ifstream image(session.actual_filepath.c_str(), std::ios::binary);
    for(size_t i = 0; i < session.num_blocks; i++){
        pkt_out.sequence++;
        pkt_out.addr = i * OCTOCLOCK_BLOCK_SIZE;

        std::cout << str(boost::format("\r -- Loading firmware: %d%% (%d/%d blocks)")
                         % int((double(i)/double(session.num_blocks))*100)
                         % i % session.num_blocks)
                  << std::flush;

        memset(pkt_out.data, 0, OCTOCLOCK_BLOCK_SIZE);
        image.read((char*)pkt_out.data, OCTOCLOCK_BLOCK_SIZE);
        UHD_OCTOCLOCK_SEND_AND_RECV(session.fw_xport, FILE_TRANSFER_CMD, pkt_out, len, session.data_in);
        if(not UHD_OCTOCLOCK_PACKET_MATCHES(FILE_TRANSFER_ACK, pkt_out, pkt_in, len)){
            image.close();
            std::cout << std::endl;
            if(session.from_hex){
                fs::remove(session.actual_filepath);
            }
            throw uhd::runtime_error("Failed to load firmware.");
        }
    }

    std::cout << str(boost::format("\r -- Loading firmware: 100%% (%d/%d blocks)")
                     % session.num_blocks % session.num_blocks)
              << std::endl;
    image.close();
}

static void octoclock_verify(octoclock_session_t &session){
    
    octoclock_packet_t pkt_out;
    pkt_out.sequence = htonx<boost::uint32_t>(std::rand());
    const octoclock_packet_t* pkt_in = reinterpret_cast<const octoclock_packet_t*>(session.data_in);
    size_t len = 0;

    std::ifstream image(session.actual_filepath.c_str(), std::ios::binary);
    boost::uint8_t image_part[OCTOCLOCK_BLOCK_SIZE];
    boost::uint16_t cmp_len = 0;
    for(size_t i = 0; i < session.num_blocks; i++){
        pkt_out.sequence++;
        pkt_out.addr = i * OCTOCLOCK_BLOCK_SIZE;

        std::cout << str(boost::format("\r -- Verifying firmware load: %d%% (%d/%d blocks)")
                         % int((double(i)/double(session.num_blocks))*100)
                         % i % session.num_blocks)
                  << std::flush;

        memset(image_part, 0, OCTOCLOCK_BLOCK_SIZE);
        image.read((char*)image_part, OCTOCLOCK_BLOCK_SIZE);
        cmp_len = image.gcount();

        UHD_OCTOCLOCK_SEND_AND_RECV(session.fw_xport, READ_FW_CMD, pkt_out, len, session.data_in);
        if(UHD_OCTOCLOCK_PACKET_MATCHES(READ_FW_ACK, pkt_out, pkt_in, len)){
            if(memcmp(pkt_in->data, image_part, cmp_len)){
                std::cout << std::endl;
                image.close();
                if(session.from_hex){
                    fs::remove(session.actual_filepath);
                }
                throw uhd::runtime_error("Failed to verify OctoClock firmware.");
            }
        }
        else{
            std::cout << std::endl;
            image.close();
            if(session.from_hex){
                fs::remove(session.actual_filepath);
            }
            throw uhd::runtime_error("Failed to verify OctoClock firmware.");
        }
    }

    image.close();
    if(session.from_hex){
        fs::remove(session.actual_filepath);
    }
    std::cout << str(boost::format("\r -- Verifying firmware load: 100%% (%d/%d blocks)")
                     % session.num_blocks % session.num_blocks)
              << std::endl;
}

static void octoclock_finalize(octoclock_session_t &session){

    octoclock_packet_t pkt_out;
    pkt_out.sequence = htonx<boost::uint32_t>(std::rand());
    const octoclock_packet_t* pkt_in = reinterpret_cast<const octoclock_packet_t*>(session.data_in);
    size_t len = 0;

    std::cout << " -- Finalizing firmware load..." << std::flush;
    UHD_OCTOCLOCK_SEND_AND_RECV(session.fw_xport, FINALIZE_BURNING_CMD, pkt_out, len, session.data_in);
    if(UHD_OCTOCLOCK_PACKET_MATCHES(FINALIZE_BURNING_ACK, pkt_out, pkt_in, len)){
        std::cout << "successful." << std::endl;
    }
    else{
        std::cout << "failed." << std::endl;
        throw uhd::runtime_error("Failed to finalize OctoClock firmware load.");
    }
}

bool octoclock_image_loader(const image_loader::image_loader_args_t &image_loader_args){
    octoclock_session_t session;
    octoclock_setup_session(session,
                            image_loader_args.args,
                            image_loader_args.firmware_path
                           );
    if(!session.found or !image_loader_args.load_firmware) return false;

    std::cout << boost::format("Unit: OctoClock (%s)")
                 % session.dev_addr["addr"]
              << std::endl;
    std::cout << "Firmware: " << session.given_filepath << std::endl;

    octoclock_burn(session);
    octoclock_verify(session);
    octoclock_finalize(session);

    return true;
}

UHD_STATIC_BLOCK(register_octoclock_image_loader){
    std::string recovery_instructions = "Aborting. Your OctoClock firmware is now corrupt. The bootloader\n"
                                        "is functional, but the device will not have functional clock distribution.\n"
                                        "Run this utility again to restore functionality or refer to:\n\n"
                                        "http://files.ettus.com/manual/page_octoclock.html\n\n"
                                        "for alternative setups.";

    image_loader::register_image_loader("octoclock",
                                        octoclock_image_loader,
                                        recovery_instructions);
}
