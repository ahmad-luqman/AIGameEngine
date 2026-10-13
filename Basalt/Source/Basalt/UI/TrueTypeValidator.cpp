#include "Basalt/UI/TrueTypeValidator.h"

#include "Basalt/Core/Assert.h"
#include "Basalt/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

// The checks follow the reads stb_truetype (imstb_truetype.h) makes through Dear ImGui's font loader
// (imgui_draw.cpp, ImGui_ImplStbTrueType_*): stbtt_GetFontOffsetForIndex, stbtt_InitFont,
// stbtt_ScaleForPixelHeight and stbtt_GetFontVMetrics (hhea), stbtt_FindGlyphIndex (cmap),
// stbtt_GetGlyphHMetrics (hmtx), stbtt_GetGlyphBitmapBox(Subpixel) (loca, glyph boxes) and
// stbtt_MakeGlyphBitmapSubpixelPrefilter (stbtt__GetGlyphShapeTT and the rasterizer). Where stb's arithmetic
// matters (16-bit wrap-around in the cmap search, the composite transform) it is mirrored exactly. Verified
// against Dear ImGui 1.92.9: check again when the imgui pin in cmake/Dependencies.cmake changes.

namespace Basalt {

	namespace {

		// A problem found while checking: thrown to the nearest boundary (the glyph being read, or the font).
		class FontError : public std::runtime_error
		{
		public:
			using std::runtime_error::runtime_error;
		};

		[[noreturn]] void Fail(const std::string& message)
		{
			throw FontError(message);
		}

		// A bounds-checked window onto the font file (a table, or one glyph inside glyf). Every read outside
		// it fails validation, naming the region.
		class Region
		{
		public:
			Region(std::span<const uint8_t> file, uint64_t offset, uint64_t length, std::string name)
				: m_Name(std::move(name))
			{
				if (offset > file.size() || length > file.size() - offset)
					Fail(fmt::format("the {} lies outside the file", m_Name));
				m_Bytes = file.subspan(static_cast<size_t>(offset), static_cast<size_t>(length));
				m_Offset = offset;
			}

			uint64_t Offset() const { return m_Offset; }
			uint64_t Length() const { return m_Bytes.size(); }

			uint8_t U8(uint64_t at) const
			{
				Require(at, 1);
				return m_Bytes[static_cast<size_t>(at)];
			}
			uint16_t U16(uint64_t at) const
			{
				Require(at, 2);
				return static_cast<uint16_t>((m_Bytes[static_cast<size_t>(at)] << 8) | m_Bytes[static_cast<size_t>(at + 1)]);
			}
			int16_t S16(uint64_t at) const { return static_cast<int16_t>(U16(at)); }
			uint32_t U32(uint64_t at) const { return (static_cast<uint32_t>(U16(at)) << 16) | U16(at + 2); }

			void Require(uint64_t at, uint64_t size) const
			{
				if (at > m_Bytes.size() || size > m_Bytes.size() - at)
					Fail(fmt::format("the {} is truncated (reading {} bytes at {} of {})", m_Name, size, at, m_Bytes.size()));
			}

		private:
			std::span<const uint8_t> m_Bytes;
			uint64_t m_Offset = 0;
			std::string m_Name;
		};

		constexpr uint32_t Tag(const char (&name)[5])
		{
			return (static_cast<uint32_t>(static_cast<uint8_t>(name[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 16) |
				   (static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(name[3]));
		}

		// Inclusive extent of coordinates, in font units.
		struct Extent
		{
			int MinX = 0;
			int MinY = 0;
			int MaxX = 0;
			int MaxY = 0;
			bool Empty = true;

			void Add(int x, int y)
			{
				if (Empty)
				{
					MinX = MaxX = x;
					MinY = MaxY = y;
					Empty = false;
					return;
				}
				MinX = std::min(MinX, x);
				MaxX = std::max(MaxX, x);
				MinY = std::min(MinY, y);
				MaxY = std::max(MaxY, y);
			}

			void Add(const Extent& other)
			{
				if (other.Empty)
					return;
				Add(other.MinX, other.MinY);
				Add(other.MaxX, other.MaxY);
			}
		};

		struct Component
		{
			uint16_t Glyph = 0;
			// stb's transform: x' = M * (a*x + c*y + e), y' = N * (b*x + d*y + f).
			float Matrix[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		};

		// What stbtt_GetGlyphShape produces for a glyph, composite glyphs expanded: the coordinates of its
		// vertices (points and midpoints between them) and of their control points (points, or the origin for
		// lines), and an upper bound on the vertices allocated.
		struct Shape
		{
			Extent Values;
			Extent Controls;
			uint64_t Vertices = 0;
			// GetGlyphShape calls made (stb expands composites recursively without caching).
			uint64_t Calls = 1;
			// Composite levels below this glyph (0 for a simple glyph).
			int Height = 0;
		};

		struct Glyph
		{
			bool Present = false; // loca gives it data (stb skips glyphs whose range is empty)
			uint64_t Offset = 0;  // in the file
			// stb cannot draw it safely; RepairTrueTypeFont blanks it.
			bool Broken = false;
			std::string Problem;
			int16_t Box[4] = {}; // xMin, yMin, xMax, yMax from the glyph header
			// A simple glyph's own outline; a composite has none (empty), only components.
			Shape Outline;
			std::vector<Component> Components;
			// The expanded shape, once resolved.
			std::optional<Shape> Resolved;
		};

		enum class ShapeStatus
		{
			Ok,
			// Expanding the glyph at this depth would nest composites too deep.
			TooDeep,
			// The glyph just turned out broken (and was marked so).
			Broken
		};

		// A shape, or why there is none.
		class ShapeResult
		{
		public:
			static ShapeResult Ok(const Shape& shape) { return { ShapeStatus::Ok, &shape }; }
			static ShapeResult TooDeep() { return { ShapeStatus::TooDeep, nullptr }; }
			static ShapeResult Broken() { return { ShapeStatus::Broken, nullptr }; }

			ShapeStatus GetStatus() const { return m_Status; }
			// Only for Ok results.
			const Shape& GetShape() const { return *m_Shape; }

		private:
			ShapeResult(ShapeStatus status, const Shape* shape)
				: m_Status(status)
				, m_Shape(shape)
			{
			}

			ShapeStatus m_Status;
			const Shape* m_Shape;
		};

		// Upper bounds on one glyph's expansion: vertices allocated, and recursive shape calls.
		constexpr uint64_t MaxShapeVertices = uint64_t{ 1 } << 18;
		constexpr uint64_t MaxShapeCalls = 4096;

		// The tables stb reads, found by ReadTableDirectory.
		struct Tables
		{
			Region Cmap;
			Region Head;
			Region Hhea;
			Region Hmtx;
			Region Glyf;
			Region Loca;
			Region Maxp;
		};

		class Validator
		{
		public:
			explicit Validator(std::span<const uint8_t> data)
				: m_Data(data)
			{
			}

			void Run()
			{
				if (m_Data.size() <= 100)
					Fail("the file is too small to be a font"); // ImGui asserts on 100 bytes or fewer
				if (m_Data.size() > MaxTrueTypeFontSize)
					Fail(fmt::format("the file is larger than {} MB", MaxTrueTypeFontSize / (size_t{ 1024 } * 1024)));

				const Region header(m_Data, 0, m_Data.size(), "file header");
				const uint32_t version = header.U32(0);
				if (version == Tag("OTTO"))
					Fail("CFF (PostScript) outlines are not supported: use a font with TrueType outlines");
				if (version == Tag("ttcf"))
					Fail("font collections (.ttc) are not supported: use a single font file");
				if (version != 0x00010000u && version != Tag("true"))
					Fail("not a TrueType font");

				ReadTableDirectory(header);
				ReadHead();
				ReadMaxp();
				ReadHorizontalMetrics();
				ReadGlyphs();
				ResolveShapes();
				ReadCmap();
			}

			std::vector<TrueTypeFontCheck::BrokenGlyph> TakeBrokenGlyphs()
			{
				std::vector<TrueTypeFontCheck::BrokenGlyph> broken;
				for (uint32_t index = 0; index < m_Glyphs.size(); index++)
				{
					Glyph& glyph = m_Glyphs[index];
					if (glyph.Broken)
						broken.push_back({ index, glyph.Offset, std::move(glyph.Problem) });
				}
				return broken;
			}

		private:
			// The tables, once ReadTableDirectory found them (every later stage runs after it).
			const Tables& GetTables() const
			{
				if (!m_Tables)
					Fail("internal error: the table directory was not read");
				return *m_Tables;
			}

			// stb_truetype finds a table by scanning the directory for the first record with its tag.
			std::optional<Region> FindTable(const Region& header, const char (&name)[5]) const
			{
				const uint32_t tag = Tag(name);
				for (uint32_t i = 0; i < m_TableCount; i++)
				{
					const uint64_t record = 12 + 16 * static_cast<uint64_t>(i);
					if (header.U32(record) != tag)
						continue;
					const uint32_t offset = header.U32(record + 8);
					const uint32_t length = header.U32(record + 12);
					// stb treats offset 0 as a missing table.
					if (offset == 0)
						return std::nullopt;
					return Region(m_Data, offset, length, fmt::format("'{}' table", std::string_view(name, 4)));
				}
				return std::nullopt;
			}

			Region RequireTable(const Region& header, const char (&name)[5]) const
			{
				std::optional<Region> table = FindTable(header, name);
				if (!table)
					Fail(fmt::format("the font has no '{}' table", std::string_view(name, 4)));
				return *table;
			}

			void ReadTableDirectory(const Region& header)
			{
				m_TableCount = header.U16(4);
				header.Require(12, 16 * static_cast<uint64_t>(m_TableCount));
				if (!FindTable(header, "glyf"))
					Fail("the font has no 'glyf' table (CFF outlines are not supported)");
				m_Tables.emplace(Tables{ RequireTable(header, "cmap"), RequireTable(header, "head"), RequireTable(header, "hhea"), RequireTable(header, "hmtx"),
										 RequireTable(header, "glyf"), RequireTable(header, "loca"), RequireTable(header, "maxp") });
			}

			void ReadHead()
			{
				m_IndexToLocFormat = GetTables().Head.U16(50);
				if (m_IndexToLocFormat > 1)
					Fail(fmt::format("unknown 'loca' format {}", m_IndexToLocFormat));
			}

			void ReadMaxp()
			{
				m_GlyphCount = GetTables().Maxp.U16(4);
				if (m_GlyphCount == 0)
					Fail("the font has no glyphs");
			}

			void ReadHorizontalMetrics()
			{
				const int ascent = GetTables().Hhea.S16(4);
				const int descent = GetTables().Hhea.S16(6);
				m_LineHeight = ascent - descent;
				// ImGui scales by 1 / (ascent - descent).
				if (m_LineHeight <= 0)
					Fail(fmt::format("the font's ascent ({}) must be above its descent ({})", ascent, descent));
				const uint16_t longMetrics = GetTables().Hhea.U16(34);
				if (longMetrics == 0 || longMetrics > m_GlyphCount)
					Fail(fmt::format("'hhea' declares {} horizontal metrics for {} glyphs", longMetrics, m_GlyphCount));
				GetTables().Hmtx.Require(0, 4 * static_cast<uint64_t>(longMetrics) + 2 * static_cast<uint64_t>(m_GlyphCount - longMetrics));
			}

			uint64_t GlyphOffset(uint32_t index) const
			{
				return m_IndexToLocFormat == 0 ? uint64_t{ GetTables().Loca.U16(2 * static_cast<uint64_t>(index)) } * 2 : GetTables().Loca.U32(4 * static_cast<uint64_t>(index));
			}

			void ReadGlyphs()
			{
				m_Glyphs.resize(m_GlyphCount);
				uint64_t previous = GlyphOffset(0);
				for (uint32_t index = 0; index < m_GlyphCount; index++)
				{
					const uint64_t next = GlyphOffset(index + 1);
					if (next < previous)
						Fail(fmt::format("'loca' offsets decrease at glyph {}", index));
					if (next > GetTables().Glyf.Length())
						Fail(fmt::format("glyph {} extends past the 'glyf' table", index));
					if (next != previous)
					{
						// The header is where a repair blanks the glyph, so it must exist.
						if (next - previous < 10)
							Fail(fmt::format("glyph {} is too short ({} bytes) for a glyph header", index, next - previous));
						const Region data(m_Data, GetTables().Glyf.Offset() + previous, next - previous, "glyph data");
						try
						{
							ReadGlyph(index, data);
						}
						catch (const FontError& error)
						{
							MarkBroken(index, std::string(error.what()));
						}
					}
					previous = next;
				}
			}

			void MarkBroken(uint32_t index, std::string problem)
			{
				Glyph& glyph = m_Glyphs[index];
				if (glyph.Broken)
					return;
				glyph.Broken = true;
				glyph.Problem = std::move(problem);
			}

			void ReadGlyph(uint32_t index, const Region& data)
			{
				Glyph& glyph = m_Glyphs[index];
				glyph.Present = true;
				glyph.Offset = data.Offset();
				const int16_t contours = data.S16(0);
				for (int i = 0; i < 4; i++)
					glyph.Box[i] = data.S16(2 + 2 * static_cast<uint64_t>(i));
				// ImGui sizes the glyph's bitmap from the box: inverted it would be negative, too big it would
				// not fit the font atlas.
				const int width = glyph.Box[2] - glyph.Box[0];
				const int height = glyph.Box[3] - glyph.Box[1];
				if (width < 0 || height < 0)
					Fail("its bounding box is inverted");
				if (width > MaxGlyphExtentPerLineHeight * m_LineHeight || height > MaxGlyphExtentPerLineHeight * m_LineHeight)
					Fail(fmt::format("it is {}x{} units, more than {} times the line height ({})", width, height, MaxGlyphExtentPerLineHeight, m_LineHeight));
				if (contours > 0)
					ReadSimpleGlyph(glyph, data, contours);
				else if (contours < 0)
					ReadCompositeGlyph(glyph, data);
			}

			void ReadSimpleGlyph(Glyph& glyph, const Region& data, int contours)
			{
				// Contour end points must increase strictly (as the format requires), so the contour starts found
				// here are where stb starts contours: the off-curve check below relies on them.
				int previousEnd = -1;
				std::vector<uint32_t> contourStarts;
				contourStarts.reserve(static_cast<size_t>(contours));
				for (int contour = 0; contour < contours; contour++)
				{
					const int end = data.U16(10 + 2 * static_cast<uint64_t>(contour));
					if (end <= previousEnd)
						Fail("its contour end points are out of order");
					contourStarts.push_back(static_cast<uint32_t>(previousEnd + 1));
					previousEnd = end;
				}
				const uint32_t points = static_cast<uint32_t>(previousEnd) + 1;
				uint64_t at = 10 + 2 * static_cast<uint64_t>(contours);
				at += 2 + data.U16(at);

				std::vector<uint8_t> flags(points);
				uint8_t flag = 0;
				uint8_t repeat = 0;
				for (uint32_t i = 0; i < points; i++)
				{
					if (repeat == 0)
					{
						flag = data.U8(at++);
						if (flag & 8)
							repeat = data.U8(at++);
					}
					else
					{
						--repeat;
					}
					flags[i] = flag;
				}

				// Coordinates are deltas, accumulated in 32 bits and stored as 16-bit values (as stb does).
				std::vector<int16_t> xs(points);
				std::vector<int16_t> ys(points);
				const auto readAxis = [&](std::vector<int16_t>& out, uint8_t shortBit, uint8_t sameBit) {
					int32_t value = 0;
					for (uint32_t i = 0; i < points; i++)
					{
						if (flags[i] & shortBit)
						{
							const int32_t delta = data.U8(at++);
							value += (flags[i] & sameBit) ? delta : -delta;
						}
						else if (!(flags[i] & sameBit))
						{
							value += data.S16(at);
							at += 2;
						}
						out[i] = static_cast<int16_t>(value);
					}
				};
				readAxis(xs, 2, 16);
				readAxis(ys, 4, 32);

				for (uint32_t i = 0; i < points; i++)
					glyph.Outline.Values.Add(xs[i], ys[i]);
				CheckTop(glyph, glyph.Outline.Values);
				// A contour that starts off the curve makes stb read the point after it; for the last point
				// there is none.
				for (uint32_t start : contourStarts)
				{
					if (start == points - 1 && !(flags[start] & 1))
						Fail("it ends with a contour of one off-curve point");
				}
				glyph.Outline.Controls = glyph.Outline.Values;
				glyph.Outline.Controls.Add(0, 0);
				// stb allocates one vertex per point plus two per contour.
				glyph.Outline.Vertices = points + 2 * static_cast<uint64_t>(contours);
			}

			void ReadCompositeGlyph(Glyph& glyph, const Region& data)
			{
				uint64_t at = 10;
				bool more = true;
				while (more)
				{
					const uint16_t flags = data.U16(at);
					Component component;
					component.Glyph = data.U16(at + 2);
					at += 4;
					if (!(flags & 2))
						Fail("it positions a component by point matching, which is not supported");
					float* matrix = component.Matrix;
					if (flags & 1)
					{
						matrix[4] = data.S16(at);
						matrix[5] = data.S16(at + 2);
						at += 4;
					}
					else
					{
						matrix[4] = static_cast<int8_t>(data.U8(at));
						matrix[5] = static_cast<int8_t>(data.U8(at + 1));
						at += 2;
					}
					const auto f2dot14 = [&](uint64_t offset) { return data.S16(offset) / 16384.0f; };
					if (flags & (1 << 3))
					{
						matrix[0] = matrix[3] = f2dot14(at);
						matrix[1] = matrix[2] = 0.0f;
						at += 2;
					}
					else if (flags & (1 << 6))
					{
						matrix[0] = f2dot14(at);
						matrix[1] = matrix[2] = 0.0f;
						matrix[3] = f2dot14(at + 2);
						at += 4;
					}
					else if (flags & (1 << 7))
					{
						matrix[0] = f2dot14(at);
						matrix[1] = f2dot14(at + 2);
						matrix[2] = f2dot14(at + 4);
						matrix[3] = f2dot14(at + 6);
						at += 8;
					}
					glyph.Components.push_back(component);
					more = (flags & (1 << 5)) != 0;
				}
			}

			// stb sizes a glyph's bitmap from its box and clamps edges that lie entirely above its top row,
			// except when that top row is row 0 (off_y == 0 in stbtt__rasterize_sorted_edges): there an edge
			// lying entirely above it trips an assertion. off_y is 0 when -1/scale < yMax <= 0, which a small
			// enough text size reaches for any box ending at or below the baseline; such an edge needs outline
			// points above the baseline (y > 0). So this conservatively breaks every glyph whose box ends at or
			// below the baseline while its outline rises above it.
			static void CheckTop(const Glyph& glyph, const Extent& values)
			{
				if (!values.Empty && glyph.Box[3] <= 0 && values.MaxY > 0)
					Fail("its outline rises above the baseline but its bounding box ends at or below it");
			}

			// Expands a glyph like stbtt_GetGlyphShape and checks each composite's transformed components.
			// Glyphs already broken are blanked by the repair, so they expand to nothing. A glyph that breaks here
			// is marked broken (a cycle breaks the glyph that comes back around), and the composites using it
			// carry on without it. One that would nest too deep at `depth` reports TooDeep to its caller, so
			// only the glyph being resolved at depth 0 is blamed (ResolveShapes) and the components it shares
			// with other glyphs stay intact.
			ShapeResult ResolveShape(uint32_t index, int depth)
			{
				static const Shape s_Empty;
				Glyph& glyph = m_Glyphs[index];
				if (!glyph.Present || glyph.Broken)
					return ShapeResult::Ok(s_Empty);
				if (glyph.Resolved)
					return depth + glyph.Resolved->Height > MaxCompositeGlyphDepth ? ShapeResult::TooDeep() : ShapeResult::Ok(*glyph.Resolved);
				if (m_OnStack[index])
				{
					MarkBroken(index, "its components form a cycle");
					return ShapeResult::Broken();
				}
				if (depth > MaxCompositeGlyphDepth)
					return ShapeResult::TooDeep();

				m_OnStack[index] = true;
				struct Unwind
				{
					std::vector<bool>::reference OnStack;
					~Unwind() { OnStack = false; }
				} unwind{ m_OnStack[index] };

				Shape shape = glyph.Outline;
				try
				{
					for (const Component& component : glyph.Components)
					{
						shape.Calls++;
						// stb draws nothing for a component past the last glyph.
						if (component.Glyph >= m_GlyphCount)
							continue;
						const ShapeResult child = ResolveShape(component.Glyph, depth + 1);
						if (child.GetStatus() == ShapeStatus::TooDeep)
							return child;
						// Broken on its own: blanked by the repair, so it draws nothing here.
						if (child.GetStatus() == ShapeStatus::Broken)
							continue;
						const Shape& part = child.GetShape();
						shape.Calls += part.Calls;
						shape.Vertices += part.Vertices;
						shape.Height = std::max(shape.Height, part.Height + 1);
						if (shape.Calls > MaxShapeCalls || shape.Vertices > MaxShapeVertices)
							Fail("it expands into too many components");
						if (part.Vertices == 0)
							continue;
						shape.Values.Add(Transform(part.Values, component.Matrix));
						shape.Controls.Add(Transform(part.Controls, component.Matrix));
					}
					CheckTop(glyph, shape.Values);
				}
				catch (const FontError& error)
				{
					MarkBroken(index, std::string(error.what()));
					return ShapeResult::Broken();
				}
				// A cycle through this glyph broke it while its components were resolved.
				if (glyph.Broken)
					return ShapeResult::Broken();
				glyph.Resolved = shape;
				return ShapeResult::Ok(*glyph.Resolved);
			}

			// stb's component transform of every corner of `extent` (the transform is linear, so the corners
			// bound it). Results must fit the 16-bit vertex coordinates stb converts them to.
			static Extent Transform(const Extent& extent, const float (&matrix)[6])
			{
				Extent result;
				if (extent.Empty)
					return result;
				const float m = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1]);
				const float n = std::sqrt(matrix[2] * matrix[2] + matrix[3] * matrix[3]);
				const int xs[2] = { extent.MinX, extent.MaxX };
				const int ys[2] = { extent.MinY, extent.MaxY };
				for (int x : xs)
				{
					for (int y : ys)
					{
						const float fx = static_cast<float>(x);
						const float fy = static_cast<float>(y);
						const float tx = m * (matrix[0] * fx + matrix[2] * fy + matrix[4]);
						const float ty = n * (matrix[1] * fx + matrix[3] * fy + matrix[5]);
						// One unit of margin: float rounding between corners and interior points.
						if (!(std::abs(tx) < 32767.0f) || !(std::abs(ty) < 32767.0f))
							Fail("it transforms a component beyond 16-bit coordinates");
						// Vertex coordinates are truncated toward zero; widen by one so the extent covers them.
						result.Add(static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)));
						result.Add(static_cast<int>(std::ceil(tx)), static_cast<int>(std::ceil(ty)));
					}
				}
				return result;
			}

			void ResolveShapes()
			{
				m_OnStack.assign(m_GlyphCount, false);
				for (uint32_t index = 0; index < m_GlyphCount; index++)
				{
					if (ResolveShape(index, 0).GetStatus() == ShapeStatus::TooDeep)
						MarkBroken(index, fmt::format("its components nest more than {} levels deep", MaxCompositeGlyphDepth));
				}
			}

			// Glyphs past the last one are fatal: stb reads their metrics past the ones 'hmtx' holds (possibly
			// past the end of the file).
			void CheckGlyphRange(uint32_t first, uint32_t last, uint64_t lastGlyph)
			{
				if (lastGlyph >= m_GlyphCount)
					Fail(fmt::format("the character map sends U+{:04X}..U+{:04X} to glyphs up to {}, but the font has {} glyphs", first, last, lastGlyph, m_GlyphCount));
			}

			void CheckGlyphId(uint32_t glyph, uint32_t codepoint)
			{
				if (glyph >= m_GlyphCount)
					Fail(fmt::format("the character map sends U+{:04X} to glyph {}, but the font has {} glyphs", codepoint, glyph, m_GlyphCount));
			}

			void ReadCmap()
			{
				const Region& cmap = GetTables().Cmap;
				const uint16_t subtables = cmap.U16(2);
				// stb takes the last Unicode subtable: Microsoft Unicode BMP or full repertoire, or any Unicode
				// platform one. Its address is a 32-bit sum, so mirror the wrap-around.
				std::optional<uint32_t> selected;
				for (uint32_t i = 0; i < subtables; i++)
				{
					const uint64_t record = 4 + 8 * static_cast<uint64_t>(i);
					const uint16_t platform = cmap.U16(record);
					const uint16_t encoding = cmap.U16(record + 2);
					if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
						selected = cmap.U32(record + 4);
				}
				if (!selected)
					Fail("the font has no Unicode character map");
				const uint32_t base = static_cast<uint32_t>(cmap.Offset());
				const uint32_t address = base + *selected;
				if (address < base || address >= base + cmap.Length())
					Fail("the font's character map lies outside the 'cmap' table");
				const uint64_t map = address - base;

				const uint16_t format = cmap.U16(map);
				switch (format)
				{
					case 0:
						ReadCmapFormat0(cmap, map);
						break;
					case 4:
						ReadCmapFormat4(cmap, map);
						break;
					case 6:
						ReadCmapFormat6(cmap, map);
						break;
					case 12:
					case 13:
						ReadCmapFormat12(cmap, map, format);
						break;
					default:
						Fail(fmt::format("character map format {} is not supported", format));
				}
			}

			void ReadCmapFormat0(const Region& cmap, uint64_t map)
			{
				const int32_t bytes = cmap.U16(map + 2);
				for (int32_t codepoint = 0; codepoint < bytes - 6; codepoint++)
					CheckGlyphId(cmap.U8(map + 6 + static_cast<uint64_t>(codepoint)), static_cast<uint32_t>(codepoint));
			}

			void ReadCmapFormat6(const Region& cmap, uint64_t map)
			{
				const uint32_t first = cmap.U16(map + 6);
				const uint32_t count = cmap.U16(map + 8);
				for (uint32_t i = 0; i < count; i++)
					CheckGlyphId(cmap.U16(map + 10 + 2 * static_cast<uint64_t>(i)), first + i);
			}

			// stbtt_FindGlyphIndex's format 4 lookup is a binary search whose parameters come from the file, so
			// where it reads depends on the codepoint. Instead of running it for each of the 65536 16-bit codepoints
			// (stb returns 0 for larger ones), this runs it on intervals of codepoints: each comparison splits an
			// interval into the codepoints that go one way and those that go the other, so every codepoint follows
			// exactly its own path, and every read stb could make is made (bounds-checked) once per interval.
			struct Format4
			{
				const Region* Cmap;
				uint64_t Map;
				uint64_t EndCount;
				uint16_t Segments;
			};

			void ReadCmapFormat4(const Region& cmap, uint64_t map)
			{
				const Format4 table{ &cmap, map, map + 14, static_cast<uint16_t>(cmap.U16(map + 6) >> 1) };
				const uint16_t searchRange = static_cast<uint16_t>(cmap.U16(map + 8) >> 1);
				const uint16_t entrySelector = cmap.U16(map + 10);
				const uint16_t rangeShift = static_cast<uint16_t>(cmap.U16(map + 12) >> 1);
				// stb accepts any value (once searchRange reaches 0, further steps re-read the same entry), but
				// SearchFormat4 recurses once per step: values above 16, which no valid font has, are rejected to
				// bound the recursion.
				if (entrySelector > 16)
					Fail(fmt::format("character map entrySelector {} is out of range", entrySelector));

				// if (codepoint >= endCode[rangeShift]) search += rangeShift * 2; then search -= 2.
				const uint64_t shifted = table.EndCount + uint64_t{ rangeShift } * 2;
				const uint32_t pivot = cmap.U16(shifted);
				if (pivot > 0)
					SearchFormat4(table, 0, std::min<uint32_t>(pivot - 1, 0xFFFF), table.EndCount - 2, entrySelector, searchRange);
				SearchFormat4(table, pivot, 0xFFFF, shifted - 2, entrySelector, searchRange);
			}

			// The codepoints [first, last] at one point of the search: `steps` halvings of `range` remain.
			void SearchFormat4(const Format4& table, uint32_t first, uint32_t last, uint64_t search, uint16_t steps, uint16_t range)
			{
				if (first > last)
					return;
				if (steps == 0)
				{
					LookUpFormat4(table, first, last, search + 2);
					return;
				}
				// range >>= 1; if (codepoint > endCode at search + range * 2) search += range * 2.
				range = static_cast<uint16_t>(range >> 1);
				const uint32_t end = table.Cmap->U16(search + uint64_t{ range } * 2);
				SearchFormat4(table, first, std::min(last, end), search, static_cast<uint16_t>(steps - 1), range);
				SearchFormat4(table, std::max(first, end + 1), last, search + uint64_t{ range } * 2, static_cast<uint16_t>(steps - 1), range);
			}

			// The end of the search for [first, last]: the segment it lands on, and the glyphs it gives.
			void LookUpFormat4(const Format4& table, uint32_t first, uint32_t last, uint64_t search)
			{
				const Region& cmap = *table.Cmap;
				const uint64_t segments = table.Segments;
				const auto item = static_cast<uint16_t>((search - table.EndCount) >> 1);
				const uint32_t start = cmap.U16(table.Map + 14 + segments * 2 + 2 + 2 * uint64_t{ item });
				const uint32_t end = cmap.U16(table.EndCount + 2 * uint64_t{ item });
				// Codepoints outside the segment give glyph 0.
				first = std::max(first, start);
				last = std::min(last, end);
				if (first > last)
					return;
				const uint64_t rangeOffsetAt = table.Map + 14 + segments * 6 + 2 + 2 * uint64_t{ item };
				const uint16_t rangeOffset = cmap.U16(rangeOffsetAt);
				if (rangeOffset == 0)
				{
					// glyph = (codepoint + idDelta) mod 65536: a run of consecutive glyphs, unless it wraps past
					// 65535 (which is never a glyph: there are at most 65535).
					const auto delta = static_cast<uint16_t>(cmap.S16(table.Map + 14 + segments * 4 + 2 + 2 * uint64_t{ item }));
					const uint32_t firstGlyph = (first + delta) & 0xFFFF;
					CheckGlyphRange(first, last, firstGlyph + uint64_t{ last - first });
					return;
				}
				for (uint32_t codepoint = first; codepoint <= last; codepoint++)
					CheckGlyphId(cmap.U16(rangeOffset + (codepoint - start) * uint64_t{ 2 } + rangeOffsetAt), codepoint);
			}

			void ReadCmapFormat12(const Region& cmap, uint64_t map, uint16_t format)
			{
				const uint32_t groups = cmap.U32(map + 12);
				cmap.Require(map + 16, uint64_t{ groups } * 12);
				for (uint32_t group = 0; group < groups; group++)
				{
					const uint64_t at = map + 16 + uint64_t{ group } * 12;
					const uint32_t start = cmap.U32(at);
					const uint32_t end = cmap.U32(at + 4);
					const uint32_t startGlyph = cmap.U32(at + 8);
					// A group whose end precedes its start never matches stb's search.
					if (end < start)
						continue;
					// stb returns startGlyph + (codepoint - start) for format 12 and startGlyph for format 13.
					CheckGlyphRange(start, end, format == 12 ? uint64_t{ startGlyph } + (end - start) : startGlyph);
				}
			}

		private:
			std::span<const uint8_t> m_Data;
			uint32_t m_TableCount = 0;
			std::optional<Tables> m_Tables;
			uint16_t m_IndexToLocFormat = 0;
			uint32_t m_GlyphCount = 0;
			int m_LineHeight = 0;
			std::vector<Glyph> m_Glyphs;
			// Glyphs whose expansion is in progress (cycle detection).
			std::vector<bool> m_OnStack;
		};

	}

	TrueTypeFontCheck CheckTrueTypeFont(std::span<const uint8_t> data)
	{
		TrueTypeFontCheck check;
		try
		{
			Validator validator(data);
			validator.Run();
			check.BrokenGlyphs = validator.TakeBrokenGlyphs();
		}
		catch (const FontError& error)
		{
			check.Error = std::string(error.what());
		}
		catch (const std::bad_alloc&)
		{
			check.Error = "out of memory while checking the font";
		}
		catch (const std::exception& error)
		{
			// No other exception is expected: a bug in the check. Debug builds (and the fuzzer) stop on it;
			// otherwise the font is rejected rather than an exception leaving the check.
			check.Error = fmt::format("internal error while checking the font: {}", error.what());
			BS_CORE_ASSERT(false, "TrueTypeValidator: {}", check.Error);
		}
		return check;
	}

	std::string ValidateTrueTypeFont(std::span<const uint8_t> data)
	{
		TrueTypeFontCheck check = CheckTrueTypeFont(data);
		if (!check.Error.empty())
			return check.Error;
		if (!check.BrokenGlyphs.empty())
			return fmt::format("glyph {}: {}", check.BrokenGlyphs.front().Index, check.BrokenGlyphs.front().Problem);
		return {};
	}

	std::string RepairTrueTypeFont(std::vector<uint8_t>& data, std::vector<TrueTypeFontCheck::BrokenGlyph>* outRepaired)
	{
		if (outRepaired)
			outRepaired->clear();
		// Blanking a glyph never breaks another one, but check again rather than trust that.
		for (int pass = 0; pass < 2; pass++)
		{
			TrueTypeFontCheck check = CheckTrueTypeFont(data);
			if (!check.Error.empty())
				return check.Error;
			if (check.BrokenGlyphs.empty())
				return {};
			for (const TrueTypeFontCheck::BrokenGlyph& glyph : check.BrokenGlyphs)
			{
				// numberOfContours = 0 and an empty box: stb draws nothing and reports no bitmap.
				std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(glyph.Offset), 10, uint8_t{ 0 });
				if (outRepaired)
					outRepaired->push_back(glyph);
			}
		}
		return ValidateTrueTypeFont(data);
	}

}
