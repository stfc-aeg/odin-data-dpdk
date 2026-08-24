#ifndef DATASOURCE_LOADER_H
#define DATASOURCE_LOADER_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <iostream>
#include <rapidjson/document.h>

#include "DataSource.h"
#include "PacketProtocolDecoder.h"

#define DATASOURCEREGISTER(Class, Name) \
    FrameProcessor::DataSourceLoader<FrameProcessor::DataSource> \
        dscl##Class(Name, FrameProcessor::data_source_maker<FrameProcessor::DataSource, Class>);

namespace FrameProcessor
{

/**
 * Function template to instantiate a DataSource class.
 */
template <typename BaseClass, typename SubClass>
boost::shared_ptr<BaseClass> data_source_maker(
    PacketProtocolDecoder* decoder,
    const rapidjson::Value& data_source_config)
{
    boost::shared_ptr<BaseClass> ptr =
        boost::shared_ptr<BaseClass>(
            new SubClass(decoder, data_source_config));

    return ptr;
}

/**
 * C++ dynamic DataSource class loader.
 */
template <typename BaseClass>
class DataSourceLoader
{
    typedef boost::shared_ptr<BaseClass> (*maker_t)(
        PacketProtocolDecoder* decoder,
        const rapidjson::Value& data_source_config);

public:

    DataSourceLoader(std::string name, maker_t value)
    {
        factory_map()[name] = value;
    }

    /**
     * Load a DataSource class given the class name and configuration.
     */
    static boost::shared_ptr<BaseClass> load_class(
        const std::string& name,
        PacketProtocolDecoder* decoder,
        const rapidjson::Value& data_source_config)
    {
        boost::shared_ptr<BaseClass> data_source;

        try
        {
            if (factory_map().count(name))
            {
                data_source = factory_map()[name](
                    decoder,
                    data_source_config);
            }
            else
            {
                std::cerr << "DataSource class not found: "
                          << name << std::endl;
            }
        }
        catch (const std::exception& ex)
        {
            std::cerr << "Error while loading DataSource class: "
                      << name
                      << ", error message: "
                      << ex.what()
                      << std::endl;
        }

        return data_source;
    }

    static void register_class(
        const std::string& name,
        maker_t maker)
    {
        factory_map()[name] = maker;
    }

    static std::map<std::string, maker_t>& factory_map()
    {
        static std::map<std::string, maker_t> factory;
        return factory;
    }

    static std::vector<std::string> get_registered_classes()
    {
        std::vector<std::string> class_names;

        for (const auto& entry : factory_map())
        {
            class_names.push_back(entry.first);
        }

        return class_names;
    }
};

}

#endif