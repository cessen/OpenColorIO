/**
 * OpenColorSpace conversion Iop.
 */

#include "ColorSpaceConversion.h"

namespace OCS = OCS_NAMESPACE;

#include <DDImage/PixelIop.h>
#include <DDImage/NukeWrapper.h>
#include <DDImage/Row.h>
#include <DDImage/Knobs.h>

#include <string>



ColorSpaceConversion::ColorSpaceConversion(Node *n) : DD::Image::PixelIop(n)
{
    hasColorSpaces = false;

    inputColorSpaceIndex = 0;
    outputColorSpaceIndex = 0;

    layersToProcess = DD::Image::Mask_RGB;

    // TODO (when to) re-grab the list of available colorspaces? How to save/load?
    try
    {
        OCS::ConstConfigRcPtr config = OCS::GetCurrentConfig();
        
        OCS::ConstColorSpaceRcPtr defaultColorSpace = \
            config->getColorSpaceForRole(OCS::ROLE_SCENE_LINEAR);

        int nColorSpaces = config->getNumColorSpaces();

        for(int i = 0; i < nColorSpaces; i++)
        {
            OCS::ConstColorSpaceRcPtr colorSpace = config->getColorSpaceByIndex(i);
            bool usedAsInput = false;
            bool isDefault = colorSpace->equals(defaultColorSpace);
            //if (colorSpace->isTransformAllowed(OCS::COLORSPACE_DIR_TO_REFERENCE))
            {
                usedAsInput = true;
                colorSpaceNames.push_back(colorSpace->getName());

                if (isDefault)
                {
                    inputColorSpaceIndex = static_cast<int>(inputColorSpaceCstrNames.size());
                }

                inputColorSpaceCstrNames.push_back(colorSpaceNames.back().c_str());
            }
            
            //if (colorSpace->isTransformAllowed(OCS::COLORSPACE_DIR_FROM_REFERENCE))
            {
                if (!usedAsInput)
                {
                    colorSpaceNames.push_back(colorSpace->getName());
                }

                if (isDefault)
                {
                    outputColorSpaceIndex = static_cast<int>(outputColorSpaceCstrNames.size());
                }

                outputColorSpaceCstrNames.push_back(colorSpaceNames.back().c_str());
            }
        }
    }
    catch (OCS::OCSException& e)
    {
        error(e.what());
    }

    hasColorSpaces = !(inputColorSpaceCstrNames.empty() || outputColorSpaceCstrNames.empty());

    inputColorSpaceCstrNames.push_back(NULL);
    outputColorSpaceCstrNames.push_back(NULL);

    if(!hasColorSpaces)
    {
        error("No ColorSpaces available for input and/or output.");
    }
}

ColorSpaceConversion::~ColorSpaceConversion()
{

}

void ColorSpaceConversion::knobs(DD::Image::Knob_Callback f)
{
    DD::Image::Enumeration_knob(f, &inputColorSpaceIndex, &inputColorSpaceCstrNames[0], "in_colorspace", "in");
    DD::Image::Tooltip(f, "Input data is taken to be in this colorspace.");

    DD::Image::Enumeration_knob(f, &outputColorSpaceIndex, &outputColorSpaceCstrNames[0], "out_colorspace", "out");
    DD::Image::Tooltip(f, "Image data is converted to this colorspace for output.");

    DD::Image::Divider(f);

    DD::Image::Input_ChannelSet_knob(f, &layersToProcess, 0, "layer", "layer");
    DD::Image::SetFlags(f, DD::Image::Knob::NO_CHECKMARKS | DD::Image::Knob::NO_ALPHA_PULLDOWN);
    DD::Image::Tooltip(f, "Set which layer to process. This should be a layer with rgb data.");
}

void ColorSpaceConversion::_validate(bool for_real)
{
    input0().validate(for_real);

    if(!hasColorSpaces)
    {
        error("No ColorSpaces available for input and/or output.");
        return;
    }

    try
    {
        const char * inputName = inputColorSpaceCstrNames[inputColorSpaceIndex];
        const char * outputName = outputColorSpaceCstrNames[outputColorSpaceIndex];
        
        OCS::ConstConfigRcPtr config = OCS::GetCurrentConfig();
        OCS::ConstColorSpaceRcPtr csSrc = config->getColorSpaceByName( inputName );
        OCS::ConstColorSpaceRcPtr csDst = config->getColorSpaceByName( outputName );
        processor = config->getProcessor(csSrc, csDst);
    }
    catch(OCS::OCSException &e)
    {
        error(e.what());
        return;
    }
    
    if(processor->isNoOp())
    {
        // TODO or call disable() ?
        set_out_channels(DD::Image::Mask_None); // prevents engine() from being called
        copy_info();
        return;
    }
    
    set_out_channels(DD::Image::Mask_All);

    DD::Image::PixelIop::_validate(for_real);
}

void ColorSpaceConversion::in_channels(int /* n unused */, DD::Image::ChannelSet& mask) const
{
    DD::Image::ChannelSet done;
    foreach(c, mask)
    {
        if ((layersToProcess & c) && DD::Image::colourIndex(c) < 3 && !(done & c))
        {
            done.addBrothers(c, 3);
        }
    }
    mask += done;
}

// See Saturation::pixel_engine for a well-commented example.
void ColorSpaceConversion::pixel_engine(
    const DD::Image::Row& in,
    int /* rowY unused */, int rowX, int rowXBound,
    const DD::Image::ChannelSet& outputChannels,
    DD::Image::Row& out)
{
    int rowWidth = rowXBound - rowX;

    DD::Image::ChannelSet done;
    foreach (requestedChannel, outputChannels)
    {
        // Skip channels which had their trios processed already,
        if (done & requestedChannel)
        {
            continue;
        }

        // Pass through channels which are not selected for processing
        // and non-rgb channels.
        if (!(layersToProcess & requestedChannel) || colourIndex(requestedChannel) >= 3)
        {
            out.copy(in, requestedChannel, rowX, rowXBound);
            continue;
        }

        DD::Image::Channel rChannel = DD::Image::brother(requestedChannel, 0);
        DD::Image::Channel gChannel = DD::Image::brother(requestedChannel, 1);
        DD::Image::Channel bChannel = DD::Image::brother(requestedChannel, 2);

        done += rChannel;
        done += gChannel;
        done += bChannel;

        const float *rIn = in[rChannel] + rowX;
        const float *gIn = in[gChannel] + rowX;
        const float *bIn = in[bChannel] + rowX;

        float *rOut = out.writable(rChannel) + rowX;
        float *gOut = out.writable(gChannel) + rowX;
        float *bOut = out.writable(bChannel) + rowX;

        #if 0
        for(int i = 0; i < rowWidth; i++)
        {
            float sum = rIn[i] + gIn[i] + bIn[i];
            sum /= 3.0f;
            rOut[i] = gOut[i] = bOut[i] = sum;
        }
        #endif

        #if 1
        // OCS modifies in-place
        memcpy(rOut, rIn, sizeof(float)*rowWidth);
        memcpy(gOut, gIn, sizeof(float)*rowWidth);
        memcpy(bOut, bIn, sizeof(float)*rowWidth);

        try
        {
            OCS::PlanarImageDesc img(rOut, gOut, bOut, rowWidth, /*height*/ 1);
            processor->render(img);
        }
        catch(OCS::OCSException &e)
        {
            error(e.what());
        }
        #endif
    }
}

const DD::Image::Op::Description ColorSpaceConversion::description("OCSColorSpaceConversion", build);

const char* ColorSpaceConversion::Class() const
{
    return description.name;
}

const char* ColorSpaceConversion::displayName() const
{
    return description.name;
}

const char* ColorSpaceConversion::node_help() const
{
    // TODO more detailed help text
    return "Use OpenColorSpace to convert from one ColorSpace to another.";
}


DD::Image::Op* build(Node *node)
{
    DD::Image::NukeWrapper *op = new DD::Image::NukeWrapper(new ColorSpaceConversion(node));
    op->noMix();
    op->noMask();
    op->noChannels(); // prefer our own channels control without checkboxes / alpha
    op->noUnpremult();
    return op;
}
