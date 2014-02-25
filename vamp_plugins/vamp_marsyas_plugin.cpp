#include "vamp_marsyas_plugin.hpp"

#include "../detector/delta_ratio.hpp"
#include "../detector/threshold.hpp"

#include <marsyas/system/MarSystemManager.h>
#include <marsyas/script/script.h>

#include <vamp-sdk/PluginAdapter.h>

#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cmath>

//FIXME: Only on POSIX:
#include <errno.h>
#include <dirent.h>

using namespace std;

namespace Marsyas {

static MarSystemManager *get_marsystem_manager()
{
  static MarSystemManager *manager = 0;
  if (!manager)
  {
    manager = new MarSystemManager;
    manager->registerPrototype("Threshold", new drum_detector::Threshold("proto_threshold"));
    manager->registerPrototype("DeltaRatio", new drum_detector::DeltaRatio("proto_delta_ratio"));
  }
  return manager;
}

class VampPluginAdapter: public Vamp::PluginAdapterBase
{
  string m_script;
public:
  VampPluginAdapter(const string & script): m_script(script) {}

private:
  virtual Vamp::Plugin * createPlugin (float inputSampleRate)
  {
    return new Marsyas::VampPlugin(m_script, inputSampleRate);
  }
};

VampPlugin::VampPlugin(const string & script, float inputSampleRate):
    Vamp::Plugin(inputSampleRate),
    m_script_filename(script),
    m_input_sample_rate(inputSampleRate),
    m_channels(0),
    m_block_size(0),
    m_step_size(0),
    m_system(0)
{
}

VampPlugin::~VampPlugin()
{
  delete m_system;
}

string VampPlugin::getIdentifier() const
{
  string id(m_script_filename);
  replace(id.begin(), id.end(), '/', '.');
  replace(id.begin(), id.end(), ' ', '.');
  return string("marsyas.") + id;
}

string VampPlugin::getName() const
{
    return string("Marsyas: ") + m_script_filename;
}

string VampPlugin::getDescription() const
{
    return "Use Marsyas script for audio processing.";
}


string VampPlugin::getMaker() const
{
    return "Jakob Leben";
}

string VampPlugin::getCopyright() const
{
    return "Copyright 2014 Jakob Leben";
}

int VampPlugin::getPluginVersion() const
{
    return 1;
}

Vamp::Plugin::InputDomain VampPlugin::getInputDomain() const
{
    return TimeDomain;
}

size_t VampPlugin::getMinChannelCount() const
{
    return 1;
}

size_t VampPlugin::getMaxChannelCount() const
{
    return 100;
}

size_t VampPlugin::getPreferredBlockSize() const
{
    return 0;
}

size_t VampPlugin::getPreferredStepSize() const
{
    return 0;
}

bool VampPlugin::initialise(size_t channels, size_t stepSize, size_t blockSize)
{
  delete m_system;

  m_channels = channels;
  m_step_size = stepSize;
  m_block_size = blockSize;

  m_system = system_from_script(m_script_filename, get_marsystem_manager());

  if (!m_system)
    return 0;

  cout << "Input format = "
       << channels
       << " / " << blockSize
       << " / " << stepSize
       << " @ " << m_input_sample_rate
       << endl;

  m_system->updControl("mrs_real/israte", (mrs_real) m_input_sample_rate);
  m_system->updControl("mrs_natural/inObservations", (mrs_natural) channels);
  m_system->updControl("mrs_natural/inSamples", (mrs_natural) blockSize);

  mrs_natural m_out_observations = m_system->getControl("mrs_natural/onObservations")->to<mrs_natural>();
  mrs_natural m_out_samples = m_system->getControl("mrs_natural/onSamples")->to<mrs_natural>();

  cout << "Output format = "
       << m_out_observations
       << " / " << m_out_samples
       << " @ " << m_system->getControl("mrs_real/osrate")->to<mrs_real>()
       << endl;

  m_input.create(m_channels, m_block_size);
  m_output.create(m_out_observations, m_out_samples);

  return true;
}

void VampPlugin::reset()
{
  initialise(m_channels, m_step_size, m_block_size);
}

Vamp::Plugin::OutputList VampPlugin::getOutputDescriptors() const
{
    OutputList outputs;

    OutputDescriptor out;
    out.identifier = "output";
    out.name = "Output";
    out.sampleType = OutputDescriptor::FixedSampleRate;
    out.hasFixedBinCount = true;
    if (m_system)
    {
      mrs_natural in_rate = m_system->getControl("mrs_natural/inSamples")->to<mrs_natural>();
      mrs_natural out_rate = m_system->getControl("mrs_natural/onSamples")->to<mrs_natural>();
      float input_output_rate_ratio = (float) out_rate / (float) in_rate;
      out.sampleRate = m_input_sample_rate * input_output_rate_ratio;
      out.binCount = m_system->getControl("mrs_natural/onObservations")->to<mrs_natural>();
    }

    outputs.push_back(out);

    return outputs;
}

Vamp::Plugin::FeatureSet VampPlugin::process(const float *const *inputBuffers, Vamp::RealTime timeStamp)
{
  Vamp::Plugin::FeatureSet feature_set;
  Vamp::Plugin::FeatureList feature_list;

  for(mrs_natural c = 0; c < m_channels; ++c)
  {
    for(mrs_natural s = 0; s < m_block_size; ++s)
    {
      m_input(c,s) = inputBuffers[c][s];
    }
  }

  m_system->process(m_input, m_output);

  for(mrs_natural s = 0; s < m_output.getCols(); ++s)
  {
    Feature features;
    for(mrs_natural o = 0; o < m_output.getRows(); ++o)
    {
      features.values.push_back( (float) m_output(o,s) );
    }

    feature_list.push_back(features);
  }

  feature_set[0] = feature_list;

  return feature_set;
}

Vamp::Plugin::FeatureSet VampPlugin::getRemainingFeatures()
{
  Vamp::Plugin::FeatureSet feature_set;
  return feature_set;
}

} // namespace Marsyas


int get_dir_entries(string dir, vector<string> &files)
{
    DIR *dp;
    struct dirent *dirp;
    if((dp  = opendir(dir.c_str())) == NULL)
    {
        cerr << "Error(" << errno << ") opening directory " << dir << endl;
        return errno;
    }

    while ((dirp = readdir(dp)) != NULL)
    {
      string dir_name(dirp->d_name);
      if (dir_name != "." && dir_name != "..")
        files.push_back(dir + '/' + dir_name);
    }
    closedir(dp);
    return 0;
}

std::vector<string> find_scripts()
{
  std::string script_location("/home/jakob/.config/marsyas/scripts");
  std::vector<string> scripts;
  get_dir_entries(script_location, scripts);
  return scripts;
}

const VampPluginDescriptor *vampGetPluginDescriptor
(unsigned int version, unsigned int index)
{
  if (version < 1) return 0;

  static bool discovery_done = false;
  static std::vector<Marsyas::VampPluginAdapter*> plugins;

  if (!discovery_done)
  {
    cout << "Discovering Marsyas scripts..." << endl;
    std::vector<string> scripts( find_scripts() );
    for(int i = 0; i < scripts.size(); ++i)
    {
      const string & script = scripts[i];
      cout << "Found Marsyas script: " << script << endl;
      plugins.push_back(new Marsyas::VampPluginAdapter(script));
    }
    cout << "Discovering Marsyas scripts done." << endl;

    discovery_done = true;
  }

  if (index < plugins.size())
  {
    return plugins[index]->getDescriptor();
  }

  return 0;
}

