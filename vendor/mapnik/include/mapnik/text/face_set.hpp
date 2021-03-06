/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2013 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#ifndef MAPNIK_FACE_SET_HPP
#define MAPNIK_FACE_SET_HPP

// mapnik
#include <mapnik/config.hpp>
#include <mapnik/noncopyable.hpp>

// fontnik
#include <fontnik/face.hpp>

// stl
#include <memory>
#include <vector>

namespace mapnik_fontnik
{

class MAPNIK_DECL font_face_set : private mapnik_fontnik::noncopyable
{
public:
    using iterator = std::vector<fontnik::face_ptr>::iterator;
    font_face_set(void) : faces_(){}

    void add(fontnik::face_ptr face);
    void set_character_sizes(double size);
    void set_unscaled_character_sizes();

    unsigned size() const { return faces_.size(); }
    iterator begin() { return faces_.begin(); }
    iterator end() { return faces_.end(); }
private:
    std::vector<fontnik::face_ptr> faces_;
};
using face_set_ptr = std::shared_ptr<font_face_set>;

} // ns mapnik

#endif // FACE_SET_HPP
