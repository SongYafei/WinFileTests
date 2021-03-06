#include "FileTransformers.h"

#include "Utils.h"

#include "WINEXCLUDE.H"
#include <windows.h>
#include <tchar.h>
#include <cstdio>
#include <memory>
#include <fstream>
#include <iostream>


///////////////////////////////////////////////////////////////////////////////
// StdioFileTransformer

bool StdioFileTransformer::Process(TProcessFunc processFunc)
{
	FILE_unique_ptr pInputFilePtr = make_fopen(m_strFirstFile.c_str(), m_useSequential ? L"rbS" : L"rb");
	if (!pInputFilePtr)
		return false;

	FILE_unique_ptr pOutputFilePtr = make_fopen(m_strSecondFile.c_str(), L"wb");
	if (!pOutputFilePtr)
		return false;

	auto inBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);
	auto outBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);
	size_t blockCount = 0;
	while (!feof(pInputFilePtr.get()))
	{
		const auto numRead = fread_s(inBuf.get(), m_blockSizeInBytes, /*element size*/sizeof(uint8_t), m_blockSizeInBytes, pInputFilePtr.get());
		
		if (numRead == 0)
		{
			if (ferror(pInputFilePtr.get()))
			{
				std::wcout << L"Couldn't read block of data (block num " << blockCount << L")!\n";
				return false;
			}

			break;
		}

		processFunc(inBuf.get(), outBuf.get(), numRead);

		const auto numWritten = fwrite(outBuf.get(), sizeof(uint8_t), numRead, pOutputFilePtr.get());
		if (numRead != numWritten)
			Logger::PrintErrorTransformingFile(numRead, numWritten);

		blockCount++;
	}

	Logger::PrintTransformSummary(blockCount, m_blockSizeInBytes, m_strFirstFile, m_strSecondFile);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// IoStreamFileTransformer

bool IoStreamFileTransformer::Process(TProcessFunc processFunc)
{
	std::ifstream inputStream(m_strFirstFile, std::ios::in | std::ios::binary);
	if (inputStream.bad())
	{
		Logger::PrintCannotOpenFile(m_strFirstFile);
		return false;
	}

	std::ofstream outputStream(m_strSecondFile, std::ios::out | std::ios::binary | std::ios::trunc);
	if (outputStream.bad())
	{
		Logger::PrintCannotOpenFile(m_strSecondFile);
		return false;
	}

	auto inBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);
	auto outBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);

	size_t blockCount = 0;
	while (!inputStream.eof())
	{
		inputStream.read((char *)(inBuf.get()), m_blockSizeInBytes);

		if (inputStream.bad())
		{
			printf("Couldn't read block of data!\n");
			return false;
		}

		const auto numRead = static_cast<size_t>(inputStream.gcount());
		if (numRead == 0)
			break;

		processFunc(inBuf.get(), outBuf.get(), static_cast<size_t>(numRead));

		const auto posBefore = outputStream.tellp();  // num of bytes written computed from file pos...
		outputStream.write((const char *)outBuf.get(), numRead);
		if (outputStream.bad())
			Logger::PrintErrorTransformingFile(numRead, static_cast<size_t>(outputStream.tellp() - posBefore));

		blockCount++;
	}

	Logger::PrintTransformSummary(blockCount, m_blockSizeInBytes, m_strFirstFile, m_strSecondFile);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// WinFileTransformer

bool WinFileTransformer::Process(TProcessFunc processFunc)
{
	auto hInputFile = make_HANDLE_unique_ptr(CreateFile(m_strFirstFile.c_str(), GENERIC_READ, /*shared mode*/0, /*security*/nullptr, OPEN_EXISTING, m_useSequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_ATTRIBUTE_NORMAL, /*template*/nullptr), m_strFirstFile);
	if (!hInputFile)
		return false;

	auto hOutputFile = make_HANDLE_unique_ptr(CreateFile(m_strSecondFile.c_str(), GENERIC_WRITE, /*shared mode*/0, /*security*/nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, /*template*/nullptr), m_strSecondFile);
	if (!hOutputFile)
		return false;

	auto inBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);
	auto outBuf = std::make_unique<uint8_t[]>(m_blockSizeInBytes);

	DWORD numBytesRead = 0;
	DWORD numBytesWritten = 0;
	size_t blockCount = 0;
	BOOL writeOK = TRUE;
	while (ReadFile(hInputFile.get(), inBuf.get(), static_cast<DWORD>(m_blockSizeInBytes), &numBytesRead, /*overlapped*/nullptr) && numBytesRead > 0 && writeOK)
	{
		processFunc(inBuf.get(), outBuf.get(), numBytesRead);

		writeOK = WriteFile(hOutputFile.get(), outBuf.get(), numBytesRead, &numBytesWritten, /*overlapped*/nullptr);
		
		if (numBytesRead != numBytesWritten)
			Logger::PrintErrorTransformingFile(numBytesRead, numBytesWritten);

		blockCount++;
	}

	Logger::PrintTransformSummary(blockCount, m_blockSizeInBytes, m_strFirstFile, m_strSecondFile);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// MappedWinFileTransformer


// with memory mapped files it's required to use SEH, so we need a separate function to do this
// see at: https://blogs.msdn.microsoft.com/larryosterman/2006/10/16/so-when-is-it-ok-to-use-seh/
bool DoProcess(uint8_t* &pIn, uint8_t* ptrInFile, uint8_t* &pOut, uint8_t* ptrOutFile, LARGE_INTEGER &fileSize, const size_t m_blockSizeInBytes, IFileTransformer::TProcessFunc processFunc)
{
	size_t bytesProcessed = 0;
	size_t blockSize = 0;

	__try
	{
		pIn = ptrInFile;
		pOut = ptrOutFile;
		while (pIn < ptrInFile + fileSize.QuadPart)
		{
			blockSize = static_cast<size_t>(static_cast<long long>(bytesProcessed + m_blockSizeInBytes) < fileSize.QuadPart ? m_blockSizeInBytes : fileSize.QuadPart - bytesProcessed);
			processFunc(pIn, pOut, blockSize);
			pIn += blockSize;
			pOut += blockSize;
			bytesProcessed += m_blockSizeInBytes;
		}
		return true;
	}
	__except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		printf("Fatal Error accessing mapped file.\n");
	}

	return false;
}

bool MappedWinFileTransformer::Process(TProcessFunc processFunc)
{
	auto hInputFile = make_HANDLE_unique_ptr(CreateFile(m_strFirstFile.c_str(), GENERIC_READ, /*shared mode*/0, /*security*/nullptr, OPEN_EXISTING, m_useSequential ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_ATTRIBUTE_NORMAL, /*template*/nullptr), m_strFirstFile);
	if (!hInputFile)
		return false;

	/* The output file MUST have Read/Write access for the mapping to succeed. */
	auto hOutputFile = make_HANDLE_unique_ptr(CreateFile(m_strSecondFile.c_str(), GENERIC_READ | GENERIC_WRITE, /*shared mode*/0, /*security*/nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, /*template*/nullptr), m_strSecondFile);
	if (!hOutputFile)
		return false;
	
	bool complete = false;
	uint8_t* ptrInFile = nullptr; 
	uint8_t* ptrOutFile = nullptr;
	uint8_t* pIn = nullptr;
	uint8_t* pOut = nullptr;

	HANDLE_unique_ptr hInputMap;
	HANDLE_unique_ptr hOutputMap;

	LARGE_INTEGER fileSize;

	/* Get the input file size. */
	GetFileSizeEx(hInputFile.get(), &fileSize);
			
	/* This is a necessar, but NOT sufficient, test for mappability on 32-bit systems S
	* Also see the long comment a few lines below */
	/*if (fileSize.HighPart > 0 && sizeof(SIZE_T) == 4)
		ReportException(_T("This file is too large to map on a Win32 system."), 4);*/

	/* Create a file mapping object on the input file. Use the file size. */
	hInputMap = make_HANDLE_unique_ptr(CreateFileMapping(hInputFile.get(), NULL, PAGE_READONLY, 0, 0, NULL), L"Input map");
	if (!hInputMap)
		return false;

	/* Map the input file */
	ptrInFile = (uint8_t*)MapViewOfFile(hInputMap.get(), FILE_MAP_READ, 0, 0, 0);
	if (ptrInFile == nullptr)
	{
		printf("Cannot map input file!\n");
		return false;
	}

	/*  Create/Open the output file. */

	hOutputMap = make_HANDLE_unique_ptr(CreateFileMapping(hOutputFile.get(), NULL, PAGE_READWRITE, fileSize.HighPart, fileSize.LowPart, NULL), L"Output map");
	if (!hOutputMap)
		return false;

	ptrOutFile = (uint8_t*)MapViewOfFile(hOutputMap.get(), FILE_MAP_WRITE, 0, 0, (SIZE_T)fileSize.QuadPart);
	if (ptrOutFile == nullptr)
	{
		printf("Cannot map output file!\n");
		UnmapViewOfFile(ptrInFile);
		return false;
	}

	DoProcess(pIn, ptrInFile, pOut, ptrOutFile, fileSize, m_blockSizeInBytes, processFunc);

	Logger::PrintTransformSummary((SIZE_T)fileSize.QuadPart/m_blockSizeInBytes, m_blockSizeInBytes, m_strFirstFile, m_strSecondFile);

	/* Close all views and handles. */
	UnmapViewOfFile(ptrOutFile); 
	UnmapViewOfFile(ptrInFile);

	return complete;
}
