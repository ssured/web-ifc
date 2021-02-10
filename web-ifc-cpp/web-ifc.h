#pragma once

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <set>

#include "../web-ifc-schema/ifc2x4.h"
#include "util.h"
#include "crack_atof.h"

namespace webifc
{
	long long ms()
	{
		using namespace std::chrono;
		milliseconds millis = duration_cast<milliseconds>(
			system_clock::now().time_since_epoch()
			);

		return millis.count();
	}

	std::vector<uint32_t> crcTable(256);

	void makeCRCTable() {
		uint32_t c;
		for (uint32_t n = 0; n < 256; n++) {
			c = n;
			for (uint32_t k = 0; k < 8; k++) {
				c = ((c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1));
			}
			crcTable[n] = c;
		}
	}

	uint32_t crc32Simple(const void* buf, size_t len)
	{
		uint32_t c = 0 ^ 0xFFFFFFFF;
		const uint8_t* u = static_cast<const uint8_t*>(buf);
		for (size_t i = 0; i < len; ++i)
		{
			c = crcTable[(c ^ u[i]) & 0xFF] ^ (c >> 8);
		}
		return c ^ 0xFFFFFFFF;
	}

	enum IfcTokenType : char
	{
		UNKNOWN,
		STRING,
		ENUM,
		REAL,
		REF,
		EMPTY,
		SET_BEGIN,
		SET_END,
		LINE_END
	};

	struct IfcPosition
	{
		uint32_t pos;
		uint32_t end;
	};

	struct IfcLine 
	{
		uint32_t expressID;
		uint32_t ifcType;
		uint32_t lineIndex;
		uint32_t tapeOffset;
	};

	class IfcLoader
	{
	public:
		void GetRefs(uint32_t id, std::unordered_map<uint32_t, std::vector<uint32_t>>& refs, std::set<uint32_t>& items)
		{
			items.insert(id);
			auto refsForId = refs[id];
			for (auto& ref : refsForId)
			{
				GetRefs(ref, refs, items);
			}
		}

		void GetRefsForLine(std::vector<uint32_t>& refs)
		{
			bool first = true;
			while (!_tape.AtEnd())
			{
				IfcTokenType t = static_cast<IfcTokenType>(_tape.Read<char>());

				switch (t)
				{
				case IfcTokenType::LINE_END:
				{
					return;
					break;
				}
				case IfcTokenType::UNKNOWN:
				case IfcTokenType::EMPTY:
				case IfcTokenType::SET_BEGIN:
				case IfcTokenType::SET_END:
					break;
				case IfcTokenType::STRING:
				case IfcTokenType::ENUM:
				{
					uint32_t start = _tape.Read<uint32_t>();
					uint32_t end = _tape.Read<uint32_t>();

					break;
				}
				case IfcTokenType::REF:
				{
					uint32_t ref = _tape.Read<uint32_t>();
					if (!first)
					{
						refs.push_back(ref);
					}
					else
					{
						first = false;
					}
					break;
				}
				case IfcTokenType::REAL:
				{
					_tape.Read<double>();
					break;
				}
				default:
					break;
				}
			}
		}

		std::unordered_map<uint32_t, std::vector<uint32_t>> GetRefs()
		{
			std::unordered_map<uint32_t, std::vector<uint32_t>> refs;

			for (auto& line : lines)
			{
				_tape.MoveTo(line.tapeOffset);
				GetRefsForLine(refs[line.expressID]);
			}

			return refs;
		}

		void GetAllRefs(std::set<uint32_t>& refs, uint32_t start)
		{
			_tape.MoveTo(lines[expressIDToLine[start]].tapeOffset);

			std::vector<uint32_t> r;
			GetRefsForLine(r);

			for (auto& ref : r)
			{
				refs.insert(ref);
				GetAllRefs(refs, ref);
			}
		}

		void LoadFile(const std::string& content)
		{
			makeCRCTable();

			buf = content.data();
			pos = 0;
			len = static_cast<uint32_t>(content.size());

			uint32_t numLines = 0;
			while (TokenizeLine())
			{
				numLines++;
			}

			std::cout << "Tape " << _tape.GetTotalSize() / 1024 / 1024 << std::endl;

			uint32_t maxExpressId = 0;
			uint32_t lineStart = 0;
			uint32_t currentIfcType = 0;
			uint32_t currentExpressID = 0;
			uint32_t currentTapeOffset = 0;
			lines.reserve(numLines);
			while (!_tape.AtEnd())
			{
				IfcTokenType t = static_cast<IfcTokenType>(_tape.Read<char>());

				switch (t)
				{
				case IfcTokenType::LINE_END:
				{
					IfcLine l;
					l.expressID = currentExpressID;
					l.ifcType = currentIfcType;
					l.lineIndex = static_cast<uint32_t>(lines.size());
					l.tapeOffset = currentTapeOffset;

					ifcTypeToLineID[l.ifcType].push_back(l.lineIndex);
					maxExpressId = std::max(maxExpressId, l.expressID);

					lines.push_back(std::move(l));

					// reset
					currentExpressID = 0;
					currentIfcType = 0;
					currentTapeOffset = _tape.GetReadOffset();

					break;
				}
				case IfcTokenType::UNKNOWN:
				case IfcTokenType::EMPTY:
				case IfcTokenType::SET_BEGIN:
				case IfcTokenType::SET_END:
					break;
				case IfcTokenType::STRING:
				case IfcTokenType::ENUM:
				{
					uint32_t start = _tape.Read<uint32_t>();
					uint32_t end = _tape.Read<uint32_t>();

					if (currentIfcType == 0)
					{
						currentIfcType = crc32Simple(&buf[start], end - start);
					}

					break;
				}
				case IfcTokenType::REF:
				{
					uint32_t ref = _tape.Read<uint32_t>();
					if (currentExpressID == 0)
					{
						currentExpressID = ref;
					}

					break;
				}
				case IfcTokenType::REAL:
				{
					_tape.Read<double>();
					break;
				}
				default:
					break;
				}
			}

			std::cout << "Lines normal " << lines.size() << std::endl;

			expressIDToLine.resize(maxExpressId + 1);

			for (int i = 0; i < lines.size(); i++)
			{
				expressIDToLine[lines[i].expressID] = i;
			}

            _open = true;

			// DumpToDisk();
		}

		void DumpToDisk()
		{
			_tape.DumpToDisk();
		}

        size_t GetNumLines()
        {
            return lines.size();
        }

		std::vector<uint32_t>& GetLineIDsWithType(uint32_t type)
		{
			return ifcTypeToLineID[type];
		}

		std::vector<uint32_t> GetExpressIDsWithType(uint32_t type)
		{
			auto& list = ifcTypeToLineID[type];
			std::vector<uint32_t> ret(list.size());

			std::transform(list.begin(), list.end(), ret.begin(), [&](uint32_t lineID) {
				return lines[lineID].expressID;
			});

			return ret;
		}

		uint32_t CopyTapeForExpressLine(uint32_t expressID, uint8_t* dest)
		{
			uint32_t startOffset = lines[expressIDToLine[expressID]].tapeOffset;
			// TODO: overflow?
			uint32_t endOffset = lines[expressIDToLine[expressID + 1]].tapeOffset;

			return _tape.Copy(startOffset, endOffset, dest);
		}

		uint32_t ExpressIDToLineID(uint32_t expressID)
		{
			return expressIDToLine[expressID];
		}

		IfcLine& GetLine(uint32_t lineID)
		{
			return lines[lineID];
		}

        bool IsOpen()
        {
            return _open;
        }

		void MoveToArgumentOffset(IfcLine& line, int argumentIndex)
		{
			_tape.MoveTo(line.tapeOffset);

			int movedOver = -3; // first is express id, second is express type, third is set begin
			bool insideSet = false; // assume no nested sets
			while (!(movedOver == argumentIndex && !insideSet))
			{
				if (!insideSet)
				{
					movedOver++;
				}

				IfcTokenType t = static_cast<IfcTokenType>(_tape.Read<char>());

				switch (t)
				{
				case IfcTokenType::LINE_END:
				{
					assert(false);
					break;
				}
				case IfcTokenType::UNKNOWN:
				case IfcTokenType::EMPTY:
					break;
				case IfcTokenType::SET_BEGIN:
					insideSet = movedOver != 0;
					break;
				case IfcTokenType::SET_END:
					insideSet = false;
					break;
				case IfcTokenType::STRING:
				case IfcTokenType::ENUM:
				{
					uint32_t start = _tape.Read<uint32_t>();
					uint32_t end = _tape.Read<uint32_t>();

					break;
				}
				case IfcTokenType::REF:
				{
					uint32_t ref = _tape.Read<uint32_t>();
					break;
				}
				case IfcTokenType::REAL:
				{
					_tape.Read<double>();
					break;
				}
				default:
					break;
				}
			}
		}

		inline void MoveToLine(uint32_t lineID)
		{
			_tape.MoveTo(lines[lineID].tapeOffset);
		}

		inline void MoveTo(uint32_t offset)
		{
			_tape.MoveTo(offset);
		}

		inline void MoveToLineArgument(uint32_t lineID, int argumentIndex)
		{
			MoveToArgumentOffset(lines[lineID], argumentIndex);
		}

		inline std::string GetStringArgument()
		{
			_tape.Read<char>(); // string type
			uint32_t start = _tape.Read<uint32_t>();
			uint32_t end = _tape.Read<uint32_t>();

			return std::string(&buf[start], end - start);
		}

		inline double GetDoubleArgument()
		{
			_tape.Read<char>(); // real type
			return _tape.Read<double>();
		}

		inline double GetDoubleArgument(uint32_t tapeOffset)
		{
			_tape.MoveTo(tapeOffset);
			return GetDoubleArgument();
		}

		inline uint32_t GetRefArgument()
		{
			_tape.Read<char>(); // ref type
			return _tape.Read<uint32_t>();
		}

		inline uint32_t GetRefArgument(uint32_t tapeOffset)
		{
			_tape.MoveTo(tapeOffset);
			return GetRefArgument();
		}

		inline IfcTokenType GetTokenType()
		{
			return static_cast<IfcTokenType>(_tape.Read<char>());
		}

		inline void Reverse()
		{
			_tape.Reverse();
		}

		inline std::vector<uint32_t> GetSetArgument()
		{
			std::vector<uint32_t> tapeOffsets;
			_tape.Read<char>(); // set begin
			int depth = 1;
			while (true)
			{
				uint32_t offset = _tape.GetReadOffset();
				IfcTokenType t = static_cast<IfcTokenType>(_tape.Read<char>());

				if (t == IfcTokenType::SET_BEGIN)
				{
					depth++;
				}
				else if (t == IfcTokenType::SET_END)
				{
					depth--;
				}
				else
				{
					tapeOffsets.push_back(offset);

					if (t == IfcTokenType::REAL)
					{
						_tape.Read<double>();
					}
					else if (t == IfcTokenType::REF)
					{
						_tape.Read<uint32_t>();
					}
					else if (t == IfcTokenType::STRING)
					{
						_tape.Read<uint32_t>();
						_tape.Read<uint32_t>();
					}
					else
					{
						assert(false);
					}
				}

				if (depth == 0)
				{
					break;
				}
			}

			return tapeOffsets;
		}

	private:
        bool _open = false;
		webifc::DynamicTape<1 << 20> _tape; // 1mb chunked tape

		bool TokenizeLine()
		{
			bool eof = false;
			bool isSTEPLine = false;
			bool firstToken = true;
			while (true)
			{
				if (pos >= len)
				{
					eof = true;
					break;
				}

				const char c = buf[pos];

				bool isWhiteSpace = c == ' ' || c == '\n' || c == '\r' || c == '\t';

				// only consider a line a stepline if the very first non-whitespace character is a ref
				if (!isWhiteSpace && firstToken && c == '#')
				{
					isSTEPLine = true;
				}
				
				if (isWhiteSpace)
				{
					pos++;
					continue;
				}

				if (!isSTEPLine)
				{
					pos++;
					continue;
				}

				if (c == '\'')
				{
					pos++;
					bool prevSlash = false;
					uint32_t start = pos;
					// apparently string escaping is not allowed in IFC ???
					// example from uptown: #114143= IFCPROPERTYSINGLEVALUE('Type Comments',$,IFCTEXT('Type G5 - 800kg/m\X2\00B2\X0\'),$);
					while (!(buf[pos] == '\'' /*&& !prevSlash*/))
					{
						if (buf[pos] == '\\')
						{
							prevSlash = !prevSlash;
						}
						else
						{
							prevSlash = false;
						}

						pos++;
					}

					_tape.push(IfcTokenType::STRING);
					_tape.push(&start, sizeof(uint32_t));
					_tape.push(&pos, sizeof(uint32_t));
				} 
				else if (c == '#')
				{
					pos++;

					uint32_t num = readInt();

					_tape.push(IfcTokenType::REF);
					_tape.push(&num, sizeof(uint32_t));
				} 
				else if (c == '$' || c == '*')
				{
					_tape.push(IfcTokenType::EMPTY);
				}
				else if (c == '(')
				{
					_tape.push(IfcTokenType::SET_BEGIN);
				}
				else if (c >= '0' && c <= '9')
				{

					bool negative = buf[pos - 1] == '-';
					double value = readDouble();
					if (negative)
					{
						value *= -1;
					}

					_tape.push(IfcTokenType::REAL);
					_tape.push(&value, sizeof(double));
				}
				else if (c == '.')
				{
					pos++;
					uint32_t start = pos;
					while (buf[pos] != '.')
					{
						pos++;
					}
					
					_tape.push(IfcTokenType::ENUM);
					_tape.push(&start, sizeof(uint32_t));
					_tape.push(&pos, sizeof(uint32_t));
				}
				else if (c >= 'A' && c <= 'Z')
				{
					uint32_t start = pos;
					while ((buf[pos] >= 'A' && buf[pos] <= 'Z') || buf[pos] >= '0' && buf[pos] <= '9')
					{
						pos++;
					}

					_tape.push(IfcTokenType::STRING);
					_tape.push(&start, sizeof(uint32_t));
					_tape.push(&pos, sizeof(uint32_t));

					pos--;
				}
				else if (c == ')')
				{
					_tape.push(IfcTokenType::SET_END);
				}
				else if (c == ';')
				{
					pos++;
					break;
				}

				pos++;
			}

			_tape.push(IfcTokenType::LINE_END);
			
			return !eof;
		}

		uint32_t readInt()
		{
			uint32_t val = 0;
			char c = buf[pos];
			
			while (c >= '0' && c <= '9')
			{
				val = val * 10 + (c - '0');
				pos++;
				c = buf[pos];
			}

			pos--;

			return val;
		}

		double readDouble()
		{
			/*
			char* end1;
			double d1 = std::strtod(&buf[pos], &end1);
			ptrdiff_t size1 = end1 - &buf[pos];
			*/

			const char* start = &buf[pos];
			const char* end = start;
			double d = crack_atof(end, &buf[len]);
			ptrdiff_t size = end - start;

			/*
			if (size1 != size)
			{
				printf("asdf");
			}
			if (std::fabs(d1 - d) > 1e-4)
			{
				printf("asdf");
			}
			*/

			pos += static_cast<uint32_t>(size - 1);

			return d;
		}
		uint32_t pos;
		uint32_t len;
		const char* buf;

		std::vector<IfcLine> lines;
		std::vector<uint32_t> expressIDToLine;
		std::unordered_map<uint32_t, std::vector<uint32_t>> ifcTypeToLineID;
	};
}