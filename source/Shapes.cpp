#include "Shapes.h"

namespace orrery
{
const char* ShapeName( Shape shape )
{
	switch( shape )
	{
	case Shape::Circle:   return "Circle";
	case Shape::Square:   return "Square";
	case Shape::Triangle: return "Triangle";
	case Shape::Hexagon:  return "Hexagon";
	case Shape::Star:     return "Star";
	case Shape::Cross:    return "Cross";
	case Shape::Ring:     return "Ring";
	case Shape::Bar:      return "Bar";
	default:              return "Circle";
	}
}

float ShapeBound( Shape shape )
{
	switch( shape )
	{
	// Inscribed in the unit circle by construction.
	case Shape::Circle:
	case Shape::Ring:
	case Shape::Star:
		return 1.0f;

	// The square SDF is built from a half-extent of 1 on each axis, so the
	// corners sit at sqrt(2). Getting this wrong is exactly the failure the
	// header describes: at 45 degrees the corners are what you see.
	case Shape::Square:
		return 1.4143f;

	// Equilateral triangle with circumradius 1: the vertices are the bound.
	case Shape::Triangle:
		return 1.0f;

	// Flat-topped hexagon with circumradius 1.
	case Shape::Hexagon:
		return 1.0f;

	// The cross arms run to 1 on each axis and the corners of the arms sit a
	// little beyond, so round up rather than assume.
	case Shape::Cross:
		return 1.05f;

	// The bar is a 1 x 0.15 box, so its corners sit a shade past 1 -- and the
	// long axis is scaled by Stretch in the shader on top of this.
	case Shape::Bar:
		return 1.05f;

	default:
		return 1.4143f;
	}
}

} // namespace orrery
