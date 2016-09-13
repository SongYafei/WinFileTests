#pragma once

#include <string>

// base class for our tests, defines basic interface and common methods
// takes two file names, transforms the first file and writes output to the second file
// transform using external function, operates on blocks of bytes
class FileTransformer
{
public:
	FileTransformer(std::wstring strFirstFile, std::wstring strSecondFile, size_t blockSizeInBytes) 
		: m_strFirstFile(std::move(strFirstFile))
		, m_strSecondFile(std::move(strSecondFile))
		, m_blockSizeInBytes(blockSizeInBytes)
	{ }

	// inBuf, outBuf, sizeInBytes: transforms inBuf and writes into outBuf, 
	// sizeInBytes should be the same as blockSizeInBytes, but might be smaller for the last block
	using TProcessFunc = bool(*) (uint8_t*, uint8_t*, size_t); 

	virtual bool Process(TProcessFunc func) = 0;

protected:
	const std::wstring m_strFirstFile;
	const std::wstring m_strSecondFile;
	const size_t m_blockSizeInBytes;
};

// transformer using STDIO, 
class StdioFileTransformer : public FileTransformer
{
public:
	using FileTransformer::FileTransformer; // inheriting constructor

	virtual bool Process(TProcessFunc func);
};

// transformer using STD library from C++, streams, 
class IoStreamFileTransformer : public FileTransformer
{
public:
	using FileTransformer::FileTransformer; // inheriting constructor

	virtual bool Process(TProcessFunc func);
};

// transformer using Windows Api, standard
class WinFileTransformer : public FileTransformer
{
public:
	using FileTransformer::FileTransformer; // inheriting constructor

	virtual bool Process(TProcessFunc func);
};

// transformer using Windows Api, memory mapped files
class MappedWinFileTransformer : public FileTransformer
{
public:
	using FileTransformer::FileTransformer; // inheriting constructor

	virtual bool Process(TProcessFunc func);
};

