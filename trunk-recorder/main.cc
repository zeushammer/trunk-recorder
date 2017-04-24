#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>

#include <boost/log/trivial.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/sinks/text_multifile_backend.hpp>

#include <boost/program_options.hpp>
#include <boost/math/constants/constants.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/tokenizer.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/foreach.hpp>


#include <iostream>
#include <cassert>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>


#include "source.h"


#include "config.h"

#include "recorders/recorder.h"
#include "recorders/p25_recorder.h"
#include "recorders/analog_recorder.h"

#include "systems/system.h"
#include "systems/smartnet_trunking.h"
#include "systems/p25_trunking.h"
#include "systems/smartnet_parser.h"
#include "systems/p25_parser.h"
#include "systems/parser.h"


#include <osmosdr/source.h>

#include <gnuradio/uhd/usrp_source.h>
#include <gnuradio/msg_queue.h>
#include <gnuradio/message.h>
#include <gnuradio/blocks/file_sink.h>
#include <gnuradio/gr_complex.h>
#include <gnuradio/top_block.h>


using namespace std;


namespace logging = boost::log;
namespace src     = boost::log::sources;
namespace sinks   = boost::log::sinks;
namespace attrs = boost::log::attributes;
namespace expr = boost::log::expressions;
namespace keywords = boost::log::keywords;

// Global logger declaration
BOOST_LOG_INLINE_GLOBAL_LOGGER_DEFAULT(my_logger, src::logger_mt)

std::vector<Source *> sources;
std::vector<System *> systems;
std::map<long, long>  unit_affiliations;

std::vector<Call *> calls;
gr::top_block_sptr  tb;
gr::msg_queue::sptr msg_queue;

volatile sig_atomic_t exit_flag = 0;
SmartnetParser *smartnet_parser;
P25Parser *p25_parser;

Config config;


string default_mode;


void exit_interupt(int sig) { // can be called asynchronously
  exit_flag = 1;              // set flag
}

unsigned GCD(unsigned u, unsigned v) {
  while (v != 0) {
    unsigned r = u % v;
    u = v;
    v = r;
  }
  return u;
}

std::vector<float>design_filter(double interpolation, double deci) {
  float beta                = 5.0;
  float trans_width         = 0.5 - 0.4;
  float mid_transition_band = 0.5 - trans_width / 2;

  std::vector<float> result = gr::filter::firdes::low_pass(
    interpolation,
    1,
    mid_transition_band / interpolation,
    trans_width / interpolation,
    gr::filter::firdes::WIN_KAISER,
    beta
    );

  return result;
}

/**
 * Method name: load_config()
 * Description: <#description#>
 * Parameters: <#parameters#>
 */
void load_config(string config_file)
{
  string system_modulation;
  int    sys_count = 0;

  try
  {
    // const std::string json_filename = "config.json";

    boost::property_tree::ptree pt;
    boost::property_tree::read_json(config_file, pt);
    BOOST_FOREACH(boost::property_tree::ptree::value_type  & node,
                  pt.get_child("systems"))
    {
      // each system should have a unique index value;
      System *system = new System(sys_count++);
      std::stringstream default_script;

      default_script << "sys_" << sys_count;

      system->set_system_type(node.second.get<std::string>("type"));
      BOOST_LOG_TRIVIAL(info) << "System Type: " << system->get_system_type();

      if (system->get_system_type() == "conventional") {
        BOOST_LOG_TRIVIAL(info) << "Conventional Channels: ";
        BOOST_FOREACH(boost::property_tree::ptree::value_type  & sub_node, node.second.get_child("channels"))
        {
          double channel = sub_node.second.get<double>("", 0);

          BOOST_LOG_TRIVIAL(info) << sub_node.second.get<double>("", 0) << " ";
          system->add_channel(channel);
        }
      }  else if (system->get_system_type() == "conventionalP25") {
        BOOST_LOG_TRIVIAL(info) << "Conventional Channels: ";
        BOOST_FOREACH(boost::property_tree::ptree::value_type  & sub_node, node.second.get_child("channels"))
        {
          double channel = sub_node.second.get<double>("", 0);

          BOOST_LOG_TRIVIAL(info) << sub_node.second.get<double>("", 0) << " ";
          system->add_channel(channel);
        }
      } else if ((system->get_system_type() == "smartnet") || (system->get_system_type() == "p25")) {
        BOOST_LOG_TRIVIAL(info) << "Control Channels: ";
        BOOST_FOREACH(boost::property_tree::ptree::value_type  & sub_node, node.second.get_child("control_channels"))
        {
          double control_channel = sub_node.second.get<double>("", 0);

          BOOST_LOG_TRIVIAL(info) << sub_node.second.get<double>("", 0) << " ";
          system->add_control_channel(control_channel);
        }
      } else {
        BOOST_LOG_TRIVIAL(error) << "System Type in config.json not recognized";
        exit(1);
      }
      BOOST_LOG_TRIVIAL(info) << "\n-------------------------------------\nSystem Number: " << sys_count;
      system->set_short_name(node.second.get<std::string>("shortName", default_script.str()));
      BOOST_LOG_TRIVIAL(info) << "Short Name: " << system->get_short_name();

      system->set_api_key(node.second.get<std::string>("apiKey", ""));
      BOOST_LOG_TRIVIAL(info) << "API Key: " << system->get_api_key();

      system->set_upload_script(node.second.get<std::string>("uploadScript", ""));
      BOOST_LOG_TRIVIAL(info) << "Upload Script: " << config.upload_script;
      system->set_call_log(node.second.get<bool>("callLog", true));
      BOOST_LOG_TRIVIAL(info) << "Call Log: " << system->get_call_log();
      system->set_audio_archive(node.second.get<bool>("audioArchive", true));
      BOOST_LOG_TRIVIAL(info) << "Audio Archive: " << system->get_audio_archive();
      system->set_talkgroups_file(node.second.get<std::string>("talkgroupsFile", ""));
      BOOST_LOG_TRIVIAL(info) << "Talkgroups File: " << system->get_talkgroups_file();
      system->set_record_unknown(node.second.get<bool>("recordUnknown",true));
      BOOST_LOG_TRIVIAL(info) << "Record Unkown Talkgroups: " << system->get_record_unknown();
      systems.push_back(system);

      system->set_bandplan(node.second.get<std::string>("bandplan", "800_standard"));
      system->set_bandfreq(800); // Default to 800

      if (boost::starts_with(system->get_bandplan(), "400")) {
        system->set_bandfreq(400);
      }
      system->set_bandplan_base(node.second.get<double>("bandplanBase", 0.0));
      system->set_bandplan_high(node.second.get<double>("bandplanHigh", 0.0));
      system->set_bandplan_spacing(node.second.get<double>("bandplanSpacing", 0.0));
      system->set_bandplan_offset(node.second.get<int>("bandplanOffset", 0));

      if (system->get_system_type() == "smartnet") {
        BOOST_LOG_TRIVIAL(info) << "Smartnet bandplan: " << system->get_bandplan();
        BOOST_LOG_TRIVIAL(info) << "Smartnet band: " << system->get_bandfreq();

        if (system->get_bandplan_base() || system->get_bandplan_spacing() || system->get_bandplan_offset()) {
          BOOST_LOG_TRIVIAL(info) << "Smartnet bandplan base freq: " << system->get_bandplan_base();
          BOOST_LOG_TRIVIAL(info) << "Smartnet bandplan high freq: " << system->get_bandplan_high();
          BOOST_LOG_TRIVIAL(info) << "Smartnet bandplan spacing: " << system->get_bandplan_spacing();
          BOOST_LOG_TRIVIAL(info) << "Smartnet bandplan offset: " << system->get_bandplan_offset();
        }
      }
      BOOST_LOG_TRIVIAL(info);
    }

    config.capture_dir = pt.get<std::string>("captureDir", boost::filesystem::current_path().string());
    size_t pos = config.capture_dir.find_last_of("/");

    if (pos == config.capture_dir.length() - 1)
    {
      config.capture_dir.erase(config.capture_dir.length() - 1);
    }
    BOOST_LOG_TRIVIAL(info) << "Capture Directory: " << config.capture_dir;
    config.upload_server = pt.get<std::string>("uploadServer", "encode-upload.sh");
    BOOST_LOG_TRIVIAL(info) << "Upload Server: " << config.upload_server;
    default_mode = pt.get<std::string>("defaultMode", "digital");
    BOOST_LOG_TRIVIAL(info) << "Default Mode: " << default_mode;
    config.call_timeout = pt.get<int>("callTimeout", 3);
    BOOST_LOG_TRIVIAL(info) << "Call Timeout (seconds): " << config.call_timeout;


    BOOST_FOREACH(boost::property_tree::ptree::value_type  & node,
                  pt.get_child("sources"))
    {
      bool   qpsk_mod       = true;
      int    silence_frames = node.second.get<int>("silenceFrames", 0);
      double center         = node.second.get<double>("center", 0);
      double rate           = node.second.get<double>("rate", 0);
      double error          = node.second.get<double>("error", 0);
      double ppm            = node.second.get<double>("ppm", 0);
      int    gain           = node.second.get<double>("gain", 0);
      int    if_gain        = node.second.get<double>("ifGain", 0);
      int    bb_gain        = node.second.get<double>("bbGain", 0);
      int    mix_gain       = node.second.get<double>("mixGain", 0);
      int    lna_gain       = node.second.get<double>("lnaGain", 0);
      double fsk_gain       = node.second.get<double>("fskGain", 1.0);
      double digital_levels = node.second.get<double>("digitalLevels", 8.0);
      double analog_levels  = node.second.get<double>("analogLevels", 8.0);
      double squelch_db     = node.second.get<double>("squelch", 0);
      std::string antenna   = node.second.get<string>("antenna", "");
      int digital_recorders = node.second.get<int>("digitalRecorders", 0);
      int debug_recorders   = node.second.get<int>("debugRecorders", 0);
      int analog_recorders  = node.second.get<int>("analogRecorders", 0);

      std::string driver = node.second.get<std::string>("driver", "");

      if ((driver != "osmosdr") && (driver != "usrp")) {
        BOOST_LOG_TRIVIAL(error) << "Driver specified in config.json not recognized, needs to be osmosdr or usrp";
      }

      std::string device = node.second.get<std::string>("device", "");

      BOOST_LOG_TRIVIAL(info) << "Center: " << node.second.get<double>("center", 0);
      BOOST_LOG_TRIVIAL(info) << "Rate: " << node.second.get<double>("rate", 0);
      BOOST_LOG_TRIVIAL(info) << "Error: " << node.second.get<double>("error", 0);
      BOOST_LOG_TRIVIAL(info) << "PPM Error: " <<  node.second.get<double>("ppm", 0);
      BOOST_LOG_TRIVIAL(info) << "Gain: " << node.second.get<double>("gain", 0);
      BOOST_LOG_TRIVIAL(info) << "IF Gain: " << node.second.get<double>("ifGain", 0);
      BOOST_LOG_TRIVIAL(info) << "BB Gain: " << node.second.get<double>("bbGain", 0);
      BOOST_LOG_TRIVIAL(info) << "LNA Gain: " << node.second.get<double>("lnaGain", 0);
      BOOST_LOG_TRIVIAL(info) << "MIX Gain: " << node.second.get<double>("mixGain", 0);
      BOOST_LOG_TRIVIAL(info) << "Squelch: " << node.second.get<double>("squelch", 0);
      BOOST_LOG_TRIVIAL(info) << "Idle Silence: " << node.second.get<bool>("idleSilence", 0);
      BOOST_LOG_TRIVIAL(info) << "Digital Recorders: " << node.second.get<int>("digitalRecorders", 0);
      BOOST_LOG_TRIVIAL(info) << "Debug Recorders: " << node.second.get<int>("debugRecorders",  0);
      BOOST_LOG_TRIVIAL(info) << "Analog Recorders: " << node.second.get<int>("analogRecorders",  0);
      BOOST_LOG_TRIVIAL(info) << "Driver: " << node.second.get<std::string>("driver",  "");

      boost::optional<std::string> mod_exists = node.second.get_optional<std::string>("modulation");

      if (mod_exists) {
        system_modulation = node.second.get<std::string>("modulation");

        if (boost::iequals(system_modulation, "qpsk"))
        {
          qpsk_mod = true;
          BOOST_LOG_TRIVIAL(info) << "Modulation: qpsk";
        } else if (boost::iequals(system_modulation, "fsk4")) {
          qpsk_mod = false;
          BOOST_LOG_TRIVIAL(info) << "Modulation: fsk4";
        } else {
          qpsk_mod = true;
          BOOST_LOG_TRIVIAL(error) << "! System Modulation Not Specified, could be fsk4 or qpsk, assuming qpsk";
        }
      } else {
        qpsk_mod = true;
      }

      if ((ppm != 0) && (error != 0)) {
        BOOST_LOG_TRIVIAL(info) <<  "Both PPM and Error should not be set at the same time. Setting Error to 0.";
        error = 0;
      }

      Source *source = new Source(center, rate, error, driver, device, &config);
      BOOST_LOG_TRIVIAL(info) << "Max HZ: " << source->get_max_hz();
      BOOST_LOG_TRIVIAL(info) << "Min HZ: " << source->get_min_hz();

      if (if_gain != 0) {
        source->set_if_gain(if_gain);
      }

      if (bb_gain != 0) {
        source->set_bb_gain(bb_gain);
      }

      if (mix_gain != 0) {
        source->set_mix_gain(mix_gain);
      }

      if (lna_gain != 0) {
        source->set_lna_gain(lna_gain);
      }
      source->set_gain(gain);
      source->set_antenna(antenna);
      source->set_squelch_db(squelch_db);
      source->set_fsk_gain(fsk_gain);
      source->set_analog_levels(analog_levels);
      source->set_digital_levels(digital_levels);
      source->set_qpsk_mod(qpsk_mod);
      source->set_silence_frames(silence_frames);

      if (ppm != 0) {
        source->set_freq_corr(ppm);
      }
      source->create_digital_recorders(tb, digital_recorders);
      source->create_analog_recorders(tb, analog_recorders);
      source->create_debug_recorders(tb, debug_recorders);
      sources.push_back(source);
    }
  }
  catch (std::exception const& e)
  {
    BOOST_LOG_TRIVIAL(error) << "Failed parsing Config: " << e.what();
  }
}

int get_total_recorders() {
  int total_recorders = 0;

  for (vector<Call *>::iterator it = calls.begin(); it != calls.end(); it++) {
    Call *call = *it;

    if (call->get_state() == recording) {
      total_recorders++;
    }
  }
  return total_recorders;
}

/**
 * Method name: start_recorder
 * Description: <#description#>
 * Parameters: TrunkMessage message
 */
bool replace(std::string& str, const std::string& from, const std::string& to) {
  size_t start_pos = str.find(from);

  if (start_pos == std::string::npos) return false;

  str.replace(start_pos, from.length(), to);
  return true;
}

void start_recorder(Call *call, TrunkMessage message, System *sys) {
  Talkgroup *talkgroup = sys->find_talkgroup(call->get_talkgroup());
  bool source_found    = false;
  bool recorder_found  = false;
  Recorder *recorder;
  Recorder *debug_recorder;

  // BOOST_LOG_TRIVIAL(info) << "\tCall created for: " << call->get_talkgroup()
  // << "\tTDMA: " << call->get_tdma() <<  "\tEncrypted: " <<
  // call->get_encrypted() << "\tFreq: " << call->get_freq();

  if (call->get_encrypted() == true) {

      BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tNot Recording: ENCRYPTED ";
      return;
    }

  if (!talkgroup && (sys->get_record_unknown() == false)) {
    BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tNot Recording: TG not in Talkgroup File ";
    return;
  }

    for (vector<Source *>::iterator it = sources.begin(); it != sources.end(); it++) {
      Source *source = *it;

      if ((source->get_min_hz() <= call->get_freq()) &&
          (source->get_max_hz() >= call->get_freq())) {
        source_found = true;

        if (talkgroup)
        {
          if (talkgroup->mode == 'A') {
            recorder = source->get_analog_recorder(talkgroup->get_priority());
          } else {
            recorder = source->get_digital_recorder(talkgroup->get_priority());
          }
        } else {
          BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tTG not in Talkgroup File ";

          // A talkgroup was not found from the talkgroup file.
          if (default_mode == "analog") {
            recorder = source->get_analog_recorder(2);
          } else {
            recorder = source->get_digital_recorder(2);
          }
        }

        int total_recorders = get_total_recorders();

        if (recorder) {
          if (message.meta.length()) {
            BOOST_LOG_TRIVIAL(trace) << message.meta;
          }
          BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tStarting Recorder on Src: " << source->get_device();

          recorder->start(call, total_recorders);
          call->set_recorder(recorder);
          call->set_state(recording);
          recorder_found = true;
        } else {
          // not recording call either because the priority was too low or no
          // recorders were available
          return;
        }

        debug_recorder = source->get_debug_recorder();

        if (debug_recorder) {
          debug_recorder->start(call, total_recorders);
          call->set_debug_recorder(debug_recorder);
          call->set_debug_recording(true);
          recorder_found = true;
        } else {
          // BOOST_LOG_TRIVIAL(info) << "\tNot debug recording call";
        }

        if (recorder_found) {
          // recording successfully started.
          return;
        }
      }
    }

    if (!source_found) {
      BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tNot Recording: no source covering Freq";

      return;
    }

}

void stop_inactive_recorders() {
  for (vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
    Call *call = *it;

    if (call->is_conventional() && call->get_recorder()) {
      // if any recording has happened
      if (call->get_current_length() > 1.0) {
        BOOST_LOG_TRIVIAL(trace) << "Recorder: " <<  call->get_current_length() << " Idle: " << call->get_recorder()->is_idle() << " Count: " << call->get_idle_count();

        // means that the squelch is on and it has stopped recording
        if (call->get_recorder()->is_idle()) {
          // increase the number of periods it has not been recording for
          call->increase_idle_count();
        } else if (call->get_idle_count() > 0) {
          // if it starts recording again, then reset the idle count
          call->reset_idle_count();
        }

        // if no additional recording has happened in the past X periods, stop and open new file
        if (call->get_idle_count() > 5) {
          call->end_call();
          call->restart_call();
        }
      }
      ++it;
    } else {
      if (call->since_last_update() > config.call_timeout) {
        call->end_call();
        it = calls.erase(it);
        delete call;
      } else {
        ++it;
      } // if rx is active
    }   // foreach loggers
  }


  /*     for (vector<Source *>::iterator it = sources.begin(); it !=
     sources.end();
           it++) {
        Source *source = *it;
        source->tune_digital_recorders();
      }*/
}

void print_status() {
  BOOST_LOG_TRIVIAL(info) << "Currently Active Calls: " << calls.size();

  for (vector<Call *>::iterator it = calls.begin(); it != calls.end(); it++) {
    Call *call         = *it;
    Recorder *recorder = call->get_recorder();
    BOOST_LOG_TRIVIAL(info) << "TG: " << call->get_talkgroup() << " Freq: " << call->get_freq() << " Elapsed: " << call->elapsed() << " State: " << call->get_state();

    if (recorder) {
      BOOST_LOG_TRIVIAL(info) << "\t[ " << recorder->get_num() << " ] State: " << recorder->get_state();
    }
  }

  BOOST_LOG_TRIVIAL(info) << "Recorders: ";

  for (vector<Source *>::iterator it = sources.begin(); it != sources.end(); it++) {
    Source *source = *it;
    source->print_recorders();
  }
}

bool retune_recorder(TrunkMessage message, Call *call) {
  Recorder *recorder = call->get_recorder();
  Source   *source   = recorder->get_source();


if (call->get_phase2_tdma() != message.phase2_tdma) {
  BOOST_LOG_TRIVIAL(info) << "\t - Retune failed, TDMA Mismatch! ";
  BOOST_LOG_TRIVIAL(info) << "\t - Message - \tTMDA: " << message.phase2_tdma << " \tSlot: " << message.tdma_slot <<"\tCall - \tTMDA: " << call->get_phase2_tdma() << "\tSlot: " << call->get_tdma_slot();
  BOOST_LOG_TRIVIAL(info) << "\t - Starting a new recording using a new recorder";
  return false;
}
if (call->get_tdma_slot() != message.tdma_slot) {
  BOOST_LOG_TRIVIAL(info) << "\t - Message - \tTMDA: " << message.phase2_tdma << " \tSlot: " << message.tdma_slot <<"\tCall - \tTMDA: " << call->get_phase2_tdma() << "\tSlot: " << call->get_tdma_slot();
  recorder->set_tdma_slot(message.tdma_slot);
  call->set_tdma_slot(message.tdma_slot);
}
  if (message.freq != call->get_freq()) {
  if ((source->get_min_hz() <= message.freq) && (source->get_max_hz() >= message.freq)) {
    recorder->tune_offset(message.freq);

      // only set the call freq, if the recorder can be retuned.
      // set the call to the new Freq / TDMA slot
      call->set_freq(message.freq);
      call->set_phase2_tdma(message.phase2_tdma);
      call->set_tdma_slot(message.tdma_slot);



    if (call->get_debug_recording() == true) {
      call->get_debug_recorder()->tune_offset(message.freq);
    }
    return true;
  } else {
    BOOST_LOG_TRIVIAL(info) << "\t - Retune failed, New Freq out of range for Source: " << source->get_device();
    BOOST_LOG_TRIVIAL(info) << "\t - Starting a new recording using a new source";
    return false;
  }
}
    return true;
}

void assign_recorder(TrunkMessage message, System *sys) {
  bool call_found = false;
  char shell_command[200];

  // go through all the talkgroups
  for (vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
    Call *call = *it;


    if (call_found && (call->get_talkgroup() == message.talkgroup) && (call->get_sys_num() == message.sys_num)) {
      BOOST_LOG_TRIVIAL(info) << "\tALERT! Assign - Total calls: " <<  calls.size() << "\tTalkgroup: " << message.talkgroup << "\tOld Freq: " << call->get_freq() << "\tNew Freq: " << message.freq;
    }

    // Does the call have the same talkgroup
    if ((call->get_talkgroup() == message.talkgroup) && (call->get_sys_num() == message.sys_num)){
      call_found = true;

      // Is the freq the same?
      if ((call->get_freq() != message.freq) || (call->get_tdma_slot() != message.tdma_slot) || (call->get_phase2_tdma() != message.phase2_tdma)) {

        // are we currently recording the call?
        if (call->get_state() == recording) {
          BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tAssign Retuning - New Freq: " << message.freq << "\tElapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update() << "s";

          int retuned = retune_recorder(message, call);

          if (!retuned) {
            // we failed to retune to the new freq, kill this call

            call->end_call();

            it = calls.erase(it);
            delete call;
            call_found = false; // since it is set to false, a new call will be
                                // started at the end of the function
          } else {
            call->update(message);
          }
        } else {
          // This call is not being recorded, just update the Freq and move on
          call->update(message);
          call->set_freq(message.freq);
          call->set_phase2_tdma(message.phase2_tdma);
          call->set_tdma_slot(message.tdma_slot);
        }
      } else {
        // The freq hasn't changed
        call->update(message);
      }

      // ++it; // move on to the next one
      // we found there has already been a Call created for the talkgroup, exit
      // the loop

      break;
    } else {
      // The talkgroup for the call does not match
      // check is the freq is the same as the one being used by the call

      /*
      if ((call->get_freq() == message.freq) &&  (call->get_tdma_slot() == message.tdma_slot)) {
        BOOST_LOG_TRIVIAL(info) << "\tFreq in use -  TG: " << message.talkgroup << "\tFreq: " << message.freq << "\tTDMA: " << message.tdma_slot << "\t Existing call\tTG: " << call->get_talkgroup() << "\tTMDA: " << call->get_tdma_slot() << "\tElapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update();

        call_found = false;
        ++it;
      } else {
        ++it; // move on to the next one
      }*/
      ++it; // move on to the next one
    }
  }


  if (!call_found) {
    Call *call = new Call(message, sys, config);
    start_recorder(call, message, sys);
    calls.push_back(call);
  }
}

void current_system_status(TrunkMessage message, System *sys) {
  sys->update_status(message);
}

void unit_registration(long unit) {}

void unit_deregistration(long unit) {
  std::map<long, long>::iterator it;

  it = unit_affiliations.find(unit);

  if (it != unit_affiliations.end()) {
    unit_affiliations.erase(it);
  }
}

void group_affiliation(long unit, long talkgroup) {
  unit_affiliations[unit] = talkgroup;
}

void update_recorder(TrunkMessage message, System *sys) {
  bool call_found = false;

  for (vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
    Call *call = *it;

    // This should help detect 2 calls being listed for the same tg
    if (call_found && (call->get_talkgroup() == message.talkgroup) && (call->get_sys_num() == message.sys_num)) {
      BOOST_LOG_TRIVIAL(info) << "\tALERT! Update - Total calls: " <<  calls.size() << "\tTalkgroup: " << message.talkgroup << "\tOld Freq: " <<  call->get_freq() << "\tNew Freq: " << message.freq;
    }

    if ((call->get_talkgroup() == message.talkgroup) && (call->get_sys_num() == message.sys_num)) {
      call_found = true;
      call->update(message);

      if ((call->get_freq() != message.freq) || (call->get_tdma_slot() != message.tdma_slot) || (call->get_phase2_tdma() != message.phase2_tdma)){
        if (call->get_state() == recording) {

          // see if we can retune the recorder, sometimes you can't if there are
          // more than one
          BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << call->get_talkgroup() << "\tFreq: " << call->get_freq() << "\tUpdate Retuning - New Freq: " << message.freq << "\tElapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update() << "s";
          int retuned = retune_recorder(message, call);

          if (!retuned) {

            call->end_call();

            it = calls.erase(it);
            delete call;
            call_found = false;
          }
        } else {
          // the Call is not recording, update and continue
          call->set_freq(message.freq);
          call->set_phase2_tdma(message.phase2_tdma);
          call->set_tdma_slot(message.tdma_slot);
        }
      }

      // we found out call, exit the for loop
      break;
    } else {
      ++it;

      // the talkgroups don't match
    }
  }

  if (!call_found) {
    BOOST_LOG_TRIVIAL(info) << "[" << sys->get_short_name() << "]\tTG: " << message.talkgroup << "\tFreq: " << message.freq << "\tCall not found for Update Message, Starting one...";

    assign_recorder(message, sys); // Treehouseman, Lets start the call if we
                                   // missed the GRANT message!
  }
}

void unit_check() {
  std::map<long, long> talkgroup_totals;
  std::map<long, long>::iterator it;
  char   shell_command[200];
  time_t starttime = time(NULL);
  tm    *ltm       = localtime(&starttime);
  char   unit_filename[160];

  std::stringstream path_stream;

  path_stream << boost::filesystem::current_path().string() <<  "/" << 1900 +  ltm->tm_year << "/" << 1 + ltm->tm_mon << "/" << ltm->tm_mday;

  boost::filesystem::create_directories(path_stream.str());


  for (it = unit_affiliations.begin(); it != unit_affiliations.end(); ++it) {
    talkgroup_totals[it->second]++;
  }

  sprintf(unit_filename, "%s/%ld-unit_check.json", path_stream.str().c_str(), starttime);

  ofstream myfile(unit_filename);

  if (myfile.is_open())
  {
    myfile << "{\n";
    myfile << "\"talkgroups\": {\n";

    for (it = talkgroup_totals.begin(); it != talkgroup_totals.end(); ++it) {
      if (it != talkgroup_totals.begin()) {
        myfile << ",\n";
      }
      myfile << "\"" << it->first << "\": " << it->second;
    }
    myfile << "\n}\n}\n";
    sprintf(shell_command, "./unit_check.sh %s > /dev/null 2>&1 &", unit_filename);
    int rc = system(shell_command);
    myfile.close();
  }
}

void handle_message(std::vector<TrunkMessage>messages, System *sys) {
  for (std::vector<TrunkMessage>::iterator it = messages.begin(); it != messages.end(); it++) {
    TrunkMessage message = *it;

    switch (message.message_type) {
    case GRANT:
      assign_recorder(message, sys);
      break;

    case UPDATE:
      update_recorder(message, sys);
      break;

    case CONTROL_CHANNEL:
      sys->add_control_channel(message.freq);
      break;

    case REGISTRATION:
      break;

    case DEREGISTRATION:
      unit_deregistration(message.source);
      break;

    case AFFILIATION:
      group_affiliation(message.source, message.talkgroup);
      break;

    case SYSID:
      break;

    case STATUS:
      current_system_status(message, sys);
      break;

    case UNKNOWN:
      break;
    }
  }
}

System* find_system(int sys_num) {
  System *sys_match = NULL;

  for (std::vector<System *>::iterator it = systems.begin(); it != systems.end(); ++it) {
    System *sys = (System *)*it;

    if (sys->get_sys_num() == sys_num) {
      sys_match = sys;
      break;
    }
  }
  return sys_match;
}

void retune_system(System *system) {
  bool source_found            = false;
  Source *source               = system->get_source();
  double  control_channel_freq = system->get_next_control_channel();

    BOOST_LOG_TRIVIAL(error) << "[" << system->get_short_name() << "] Retuning to Control Channel: " << control_channel_freq;
    BOOST_LOG_TRIVIAL(info) << "\t - System Source - Min Freq: " << source->get_min_hz() << " Max Freq: " << source->get_max_hz();

    if ((source->get_min_hz() <= control_channel_freq) &&
        (source->get_max_hz() >= control_channel_freq)) {
      // The source can cover the System's control channel, break out of the
      // For Loop
    if (system->get_system_type() == "smartnet") {
      // what you really need to do is go through all of the sources to find
      // the one with the right frequencies
      system->smartnet_trunking->tune_offset(control_channel_freq);
    } else if (system->get_system_type() == "p25") {
      // what you really need to do is go through all of the sources to find
      // the one with the right frequencies
      system->p25_trunking->tune_offset(control_channel_freq);
    } else {
      BOOST_LOG_TRIVIAL(error) << "\t - Unkown system type for Retune";
    }
  } else {
    BOOST_LOG_TRIVIAL(error) << "\t - Unable to retune System control channel, freq not covered by the Source used for the inital control channel freq.";
  }
}

void check_message_count(float timeDiff) {
  for (std::vector<System *>::iterator it = systems.begin(); it != systems.end(); ++it) {
    System *sys = (System *)*it;

    if ((sys->system_type != "conventional") && (sys->system_type != "conventionalP25")) {
      float msgs_decoded_per_second = sys->message_count / timeDiff;

      if (msgs_decoded_per_second < 2) {

        if (sys->system_type == "smartnet") {
          sys->smartnet_trunking->reset();
        }
        if (sys->control_channel_count() > 1) {
          retune_system(sys);
        } else {
          BOOST_LOG_TRIVIAL(error) << "[" << sys->get_short_name() << "]\tThere is only one control channel defined";
        }
      }

      if (msgs_decoded_per_second < 10) {
        BOOST_LOG_TRIVIAL(error) << "[" << sys->get_short_name() << "]\t Control Channel Message Decode Rate: " <<  msgs_decoded_per_second << "/sec, count:  " << sys->message_count;
      }
      sys->message_count = 0;
    }
  }
}

void monitor_messages() {
  gr::message::sptr msg;
  int sys_num;
  System *sys;

  time_t lastStatusTime     = time(NULL);
  time_t lastMsgCountTime   = time(NULL);
  time_t lastTalkgroupPurge = time(NULL);
  time_t currentTime        = time(NULL);
  std::vector<TrunkMessage> trunk_messages;

  while (1) {
    currentTime = time(NULL);

    if (exit_flag) { // my action when signal set it 1
      printf("\n Signal caught!\n");
      return;
    }


    // BOOST_LOG_TRIVIAL(info) << "Messages waiting: "  << msg_queue->count();
    msg = msg_queue->delete_head_nowait();


    if ((currentTime - lastTalkgroupPurge) >= 1.0)
    {
      stop_inactive_recorders();
      lastTalkgroupPurge = currentTime;
    }

    if (msg != 0) {
      sys_num = msg->arg1();
      sys     = find_system(sys_num);
      sys->message_count++;

      if (sys) {
        if (sys->get_system_type() == "smartnet") {
          trunk_messages = smartnet_parser->parse_message(msg->to_string(), sys);
          handle_message(trunk_messages, sys);
        }

        if (sys->get_system_type() == "p25") {
          trunk_messages = p25_parser->parse_message(msg);
          handle_message(trunk_messages, sys);
        }
      }

      /*
              if ((currentTime - lastUnitCheckTime) >= 300.0) {
                  unit_check();
                  lastUnitCheckTime = currentTime;
              }
       */

      msg.reset();
    } else {
      usleep(1000 * 10);
    }
    float timeDiff = currentTime - lastMsgCountTime;

    if (timeDiff >= 3.0) {
      check_message_count(timeDiff);
      lastMsgCountTime = currentTime;
    }

    float statusTimeDiff = currentTime - lastStatusTime;

    if (statusTimeDiff > 300) {
      lastStatusTime = currentTime;
      print_status();
    }
  }
}

bool monitor_system() {
  bool system_added = false;
  Source *source    = NULL;

  for (vector<System *>::iterator sys_it = systems.begin(); sys_it != systems.end(); sys_it++) {
    System *system       = *sys_it;
    bool    source_found = false;


    if ((system->get_system_type() == "conventional") || (system->get_system_type() == "conventionalP25")) {
      std::vector<double> channels = system->get_channels();
      int talkgroup                = 1;

      for (vector<double>::iterator chan_it = channels.begin(); chan_it != channels.end(); chan_it++) {
        double channel = *chan_it;

        for (vector<Source *>::iterator src_it = sources.begin(); src_it != sources.end(); src_it++) {
          source = *src_it;

          if ((source->get_min_hz() <= channel) &&
              (source->get_max_hz() >= channel)) {
            // The source can cover the System's control channel
            system->set_source(source);
            system_added = true;

            if (source->get_squelch_db() == 0) {
              BOOST_LOG_TRIVIAL(error) << "[" << system->get_short_name() << "]\tSquelch needs to be specified for the Source for Conventional Systems";
              system_added = false;
            } else {
              system_added = true;
            }

            BOOST_LOG_TRIVIAL(info) << "[" << system->get_short_name() << "]\tMonitoring Conventional Channel: " << channel << " Talkgroup: " << talkgroup;
            Call *call = new Call(talkgroup, channel, system, config);
            talkgroup++;
            call->set_conventional(true);

            if (system->get_system_type() == "conventional") {
              analog_recorder_sptr rec;
              rec = source->create_conventional_recorder(tb);
              rec->start(call, talkgroup + 100);
              call->set_recorder((Recorder *)rec.get());
              call->set_state(recording);
              system->add_conventional_recorder(rec);
              calls.push_back(call);
            } else { // has to be "conventionalP25"
              p25_recorder_sptr rec;
              rec = source->create_conventionalP25_recorder(tb);
              rec->start(call, talkgroup + 100);
              call->set_recorder((Recorder *)rec.get());
              call->set_state(recording);
              system->add_conventionalP25_recorder(rec);
              calls.push_back(call);
            }

            // break out of the for loop
            break;
          }
        }
      }
    } else {
      double control_channel_freq = system->get_current_control_channel();
      BOOST_LOG_TRIVIAL(info) << "[" << system->get_short_name() << "]\tStarted with Control Channel: " << control_channel_freq;

      for (vector<Source *>::iterator src_it = sources.begin(); src_it != sources.end(); src_it++) {
        source = *src_it;

        if ((source->get_min_hz() <= control_channel_freq) &&
            (source->get_max_hz() >= control_channel_freq)) {
          // The source can cover the System's control channel
          system_added = true;
          system->set_source(source);

          if (system->get_system_type() == "smartnet") {
            // what you really need to do is go through all of the sources to find
            // the one with the right frequencies
            system->smartnet_trunking = make_smartnet_trunking(control_channel_freq,
                                                               source->get_center(),
                                                               source->get_rate(),
                                                               msg_queue,
                                                               system->get_sys_num());
            tb->connect(source->get_src_block(), 0, system->smartnet_trunking, 0);
          }

          if (system->get_system_type() == "p25") {
            // what you really need to do is go through all of the sources to find
            // the one with the right frequencies
            system->p25_trunking = make_p25_trunking(control_channel_freq,
                                                     source->get_center(),
                                                     source->get_rate(),
                                                     msg_queue,
                                                     source->get_qpsk_mod(),
                                                     system->get_sys_num());
            tb->connect(source->get_src_block(), 0, system->p25_trunking, 0);
          }

          // break out of the For Loop
          break;
        }
      }
    }
  }
  return system_added;
}

template <class F>
void add_logs(const F & fmt)
{
  boost::log::add_console_log(std::clog, boost::log::keywords::format = fmt);
}

void init_logging()
{
  try
  {
      // Create a text file sink
      typedef sinks::synchronous_sink< sinks::text_multifile_backend > file_sink;
      boost::shared_ptr< file_sink > sink(new file_sink);

      // Set up how the file names will be generated
      sink->locked_backend()->set_file_name_composer(sinks::file::as_file_name_composer(
          expr::stream << "logs/" << expr::attr< boost::thread::id >("ThreadID") << ".log"));

      // Set the log record formatter
      sink->set_formatter
      (
          expr::format("%1%: [%2%] - %3%")
              % expr::attr< unsigned int >("RecordID")
              % expr::attr< boost::posix_time::ptime >("TimeStamp")
              % expr::smessage
      );

      // Add it to the core
      logging::core::get()->add_sink(sink);

      // Add some attributes too
      logging::core::get()->add_global_attribute("TimeStamp", attrs::local_clock());
      logging::core::get()->add_global_attribute("RecordID", attrs::counter< unsigned int >());

  }
  catch (std::exception& e)
  {
      std::cout << "FAILURE: " << e.what() << std::endl;
  }
}

int main(int argc, char **argv)
{
  BOOST_STATIC_ASSERT(true) __attribute__((unused));
  signal(SIGINT, exit_interupt);
  /*logging::core::get()->set_filter
  (
    logging::trivial::severity >= logging::trivial::info

  );

  boost::log::add_common_attributes();
  boost::log::core::get()->add_global_attribute("Scope",
                                                boost::log::attributes::named_scope());
  boost::log::core::get()->set_filter(
    boost::log::trivial::severity >= boost::log::trivial::info
    );

add_logs(
  boost::log::expressions::format("[%1%] (%2%)   %3%")
  % boost::log::expressions::
                      format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
  % boost::log::expressions::
                     attr<boost::log::trivial::severity_level>("Severity")
  % boost::log::expressions::smessage
);
*/
init_logging();
BOOST_LOG_SCOPED_THREAD_TAG("ThreadID", boost::this_thread::get_id());
for (unsigned int i = 0; i < 5; ++i)
{
    BOOST_LOG(my_logger::get()) << "Log record " << i;
}

  boost::program_options::options_description desc("Options");
  desc.add_options()
    ("help,h", "Help screen")
    ("config", boost::program_options::value<string>()->default_value("./config.json"), "Config File");
  boost::program_options::variables_map vm;
  boost::program_options::store(parse_command_line(argc, argv, desc), vm);
  boost::program_options::notify(vm);

  if (vm.count("help")) {
    std::cout << "Usage: options_description [options]\n";
    std::cout << desc;
    exit(0);
  }
  string config_file = vm["config"].as<string>();

  if (vm.count("config"))
  {
    BOOST_LOG_TRIVIAL(info) << "Using Config file: " << config_file << "\n";
  }


  tb              = gr::make_top_block("Trunking");
  msg_queue       = gr::msg_queue::make(100);
  smartnet_parser = new SmartnetParser(); // this has to eventually be generic;
  p25_parser      = new P25Parser();




  load_config(config_file);


  if (monitor_system()) {

    tb->start();

    monitor_messages();

    // ------------------------------------------------------------------
    // -- stop flow graph execution
    // ------------------------------------------------------------------
    BOOST_LOG_TRIVIAL(info) << "stopping flow graph";
    tb->stop();
    tb->wait();
  } else {
    BOOST_LOG_TRIVIAL(error) << "Unable to setup a System to record, exiting..." <<  std::endl;
  }

  return 0;
}
