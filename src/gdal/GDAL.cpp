/* <editor-fold desc="MIT License">

Copyright(c) 2021 Robert Osfield

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

</editor-fold> */

#include <vsg/io/Logger.h>
#include <vsgXchange/gdal.h>

#include <cstring>
#include <sstream>

using namespace vsgXchange;

namespace vsgXchange
{

    class GDAL::Implementation
    {
    public:
        Implementation();

        vsg::ref_ptr<vsg::Object> read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options = {}) const;
        vsg::ref_ptr<vsg::Object> read(std::istream& fin, vsg::ref_ptr<const vsg::Options> options) const;
        vsg::ref_ptr<vsg::Object> read(const uint8_t* ptr, size_t size, vsg::ref_ptr<const vsg::Options> options) const;

    protected:
    };

} // namespace vsgXchange

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// GDAL ReaderWriter facade
//
GDAL::GDAL() :
    _implementation(new GDAL::Implementation())
{
}

GDAL::~GDAL()
{
    delete _implementation;
}

vsg::ref_ptr<vsg::Object> GDAL::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    return _implementation->read(filename, options);
}

vsg::ref_ptr<vsg::Object> GDAL::read(std::istream& fin, vsg::ref_ptr<const vsg::Options> options) const
{
    return _implementation->read(fin, options);
}
vsg::ref_ptr<vsg::Object> GDAL::read(const uint8_t* ptr, size_t size, vsg::ref_ptr<const vsg::Options> options) const
{
    return _implementation->read(ptr, size, options);
}

bool GDAL::getFeatures(Features& features) const
{
    vsgXchange::initGDAL();

    auto driverManager = GetGDALDriverManager();
    int driverCount = driverManager->GetDriverCount();

    vsg::ReaderWriter::FeatureMask rasterFeatureMask = vsg::ReaderWriter::READ_FILENAME;

    const std::string dotPrefix = ".";

    for (int i = 0; i < driverCount; ++i)
    {
        auto driver = driverManager->GetDriver(i);
        auto raster_meta = driver->GetMetadataItem(GDAL_DCAP_RASTER);
        auto extensions_meta = driver->GetMetadataItem(GDAL_DMD_EXTENSIONS);
        // auto longname_meta = driver->GetMetadataItem( GDAL_DMD_LONGNAME );
        if (raster_meta && extensions_meta)
        {
            std::string extensions = extensions_meta;
            std::string ext;

            std::string::size_type start_pos = 0;
            for (;;)
            {
                start_pos = extensions.find_first_not_of(" .", start_pos);
                if (start_pos == std::string::npos) break;

                std::string::size_type delimiter_pos = extensions.find_first_of(" /", start_pos);
                if (delimiter_pos != std::string::npos)
                {
                    ext = extensions.substr(start_pos, delimiter_pos - start_pos);
                    features.extensionFeatureMap[dotPrefix + ext] = rasterFeatureMask;
                    start_pos = delimiter_pos + 1;
                    if (start_pos == extensions.length()) break;
                }
                else
                {
                    ext = extensions.substr(start_pos, std::string::npos);
                    features.extensionFeatureMap[dotPrefix + ext] = rasterFeatureMask;
                    break;
                }
            }
        }
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// GDAL ReaderWriter implementation
//
GDAL::Implementation::Implementation()
{
}

vsg::ref_ptr<vsg::Object> GDAL::Implementation::read(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options) const
{
    // GDAL tries to load all datatypes so up front catch VSG and OSG native formats.
    vsg::Path ext = vsg::lowerCaseFileExtension(filename);
    if (ext == ".vsgb" || ext == ".vsgt" || ext == ".osgb" || ext == ".osgt" || ext == ".osg" || ext == ".tile") return {};

    vsg::Path filenameToUse = (vsg::filePath(filename) == "/vsimem") ? filename : vsg::findFile(filename, options);

    if (!filenameToUse) return {};

    vsgXchange::initGDAL();

    auto dataset = vsgXchange::openSharedDataSet(filenameToUse, GA_ReadOnly);
    if (!dataset)
    {
        return {};
    }

    auto types = vsgXchange::dataTypes(*dataset);
    if (types.size() > 1)
    {
        vsg::info("GDAL::read(", filename, ") multiple input data types not supported.");
        for (auto& type : types)
        {
            vsg::info("   GDALDataType ", GDALGetDataTypeName(type));
        }
        return {};
    }
    if (types.empty())
    {
        vsg::info("GDAL::read(", filename, ") types set empty.");

        return {};
    }

    GDALDataType dataType = *types.begin();

    std::vector<GDALRasterBand*> rasterBands;
    for (int i = 1; i <= dataset->GetRasterCount(); ++i)
    {
        GDALRasterBand* band = dataset->GetRasterBand(i);
        GDALColorInterp classification = band->GetColorInterpretation();

        if (classification != GCI_Undefined)
        {
            rasterBands.push_back(band);
        }
        else
        {
            vsg::info("GDAL::read(", filename, ") Undefined classification on raster band ", i);
        }
    }

    int numComponents = static_cast<int>(rasterBands.size());
    if (numComponents == 0)
    {
        vsg::info("GDAL::read(", filename, ") failed numComponents = ", numComponents);
        return {};
    }

    bool mapRGBtoRGBAHint = !options || options->mapRGBtoRGBAHint;
    if (mapRGBtoRGBAHint && numComponents == 3)
    {
        //std::cout<<"Remapping RGB to RGBA "<<filename<<std::endl;
        numComponents = 4;
    }

    if (numComponents > 4)
    {
        vsg::info("GDAL::read(", filename, ") Too many raster bands to merge into a single output, maximum of 4 raster bands supported.");
        return {};
    }

    int width = dataset->GetRasterXSize();
    int height = dataset->GetRasterYSize();

    auto image = vsgXchange::createImage2D(width, height, numComponents, dataType, vsg::dvec4(0.0, 0.0, 0.0, 1.0));
    if (!image) return {};

    for (int component = 0; component < static_cast<int>(rasterBands.size()); ++component)
    {
        vsgXchange::copyRasterBandToImage(*rasterBands[component], *image, component);
    }

    vsgXchange::assignMetaData(*dataset, *image);

    if (dataset->GetProjectionRef() && std::strlen(dataset->GetProjectionRef()) > 0)
    {
        image->setValue("ProjectionRef", std::string(dataset->GetProjectionRef()));
    }

    auto transform = vsg::doubleArray::create(6);
    if (dataset->GetGeoTransform(transform->data()) == CE_None)
    {
        image->setObject("GeoTransform", transform);
    }

    return image;
}

vsg::ref_ptr<vsg::Object> GDAL::Implementation::read(std::istream& fin, vsg::ref_ptr<const vsg::Options> options) const
{
    // if (!vsg::compatibleExtension(options, _supportedExtensions)) return {};

    std::string input;
    std::stringstream* sstr = dynamic_cast<std::stringstream*>(&fin);
    if (sstr)
    {
        input = sstr->str();
    }
    else
    {
        std::string buffer(1 << 16, 0); // 64kB
        while (!fin.eof())
        {
            fin.read(&buffer[0], buffer.size());
            const auto bytes_readed = fin.gcount();
            input.append(&buffer[0], bytes_readed);
        }
    }

    return read(reinterpret_cast<const uint8_t*>(input.data()), input.size(), options);
}

vsg::ref_ptr<vsg::Object> GDAL::Implementation::read(const uint8_t* ptr, size_t size, vsg::ref_ptr<const vsg::Options> options) const
{
    std::string temp_filename("/vsimem/temp");
    temp_filename.append(options->extensionHint.string());

    // create a GDAL Virtual File for memory block.
    VSILFILE* vsFile = VSIFileFromMemBuffer(temp_filename.c_str(), static_cast<GByte*>(const_cast<uint8_t*>(ptr)), static_cast<vsi_l_offset>(size), 0);

    auto result = GDAL::Implementation::read(temp_filename, options);

    VSIFCloseL(vsFile);

    return result;
}
