#include "dancegraph.hh"

#include "fs.hh"
#include "notes.hh"
#include "surface.hh"
#include <boost/lexical_cast.hpp>
#include <stdexcept>
#include <algorithm>

namespace {
	const std::string diffv[] = { "Beginner", "Easy", "Medium", "Hard", "Challenge" };
	const float death_delay = 20.0f; // Delay in seconds after which the player is hidden
	const float not_joined = -100; // A value that indicates player hasn't joined
	const float join_delay = 5.0f; // Time to select track/difficulty when joining mid-game
	const float past = -0.4f;
	const float future = 2.0f;
	const float timescale = 7.0f;
	// Note: t is difference from playback time so it must be in range [past, future]
	float time2y(float t) { return timescale * (t - past) / (future - past); }
	float time2a(float t) {
		float a = clamp(1.0 - t / future); // Note: we want 1.0 alpha already at zero t.
		return std::pow(a, 0.8f); // Nicer curve
	}
	float y2a(float y) { return time2a(past - y / timescale * (future - past)); }
	const double maxTolerance = 0.15;
	int getNextBigStreak(int prev) { return prev + 10; }
	
	/// Gives points based on error from a perfect hit
	double points(double error) {
		error = (error < 0.0) ? -error : error;
		double score = 0.0;
		if (error < maxTolerance) score += 15;
		if (error < maxTolerance / 2) score += 15;
		if (error < maxTolerance / 4) score += 15;
		if (error < maxTolerance / 6) score += 5;
		return score;
	}
	struct lessEnd {
		bool operator()(const DanceNote& left, const DanceNote& right) {
			return left.note.end < right.note.end;
		}
	};
}


/// Constructor
DanceGraph::DanceGraph(Audio& audio, Song const& song):
  m_level(BEGINNER),
  m_audio(audio),
  m_song(song),
  m_input(input::DANCEPAD),
  m_arrows(getThemePath("arrows.svg")),
  m_arrows_cursor(getThemePath("arrows_cursor.svg")),
  m_arrows_hold(getThemePath("arrows_hold.svg")),
  m_mine(getThemePath("mine.svg")),
  m_cx(0.0, 0.2),
  m_width(0.5, 0.4),
  m_stream(),
  m_text(getThemePath("sing_timetxt.svg"), config["graphic/text_lod"].f()),
  m_correctness(0.0, 5.0),
  m_streakPopup(0.0, 1.0),
  m_flow_direction(1),
  m_score(),
  m_scoreFactor(1),
  m_streak(),
  m_longestStreak(),
  m_bigStreak(),
  m_jointime(not_joined),
  m_acttime()
{
	m_popupText.reset(new SvgTxtThemeSimple(getThemePath("sing_score_text.svg"), config["graphic/text_lod"].f()));

	// Initialize some arrays
	for(size_t i=0; i < 4; i++) m_activeNotes[i] = m_notes.end();
	for(size_t i = 0; i < 4; i++) m_pressed[i] = false;
	for(size_t i = 0; i < 4; i++) m_pressed_anim[i] = AnimValue(0.0, 4.0);
	
	if(m_song.danceTracks.empty())
		throw std::runtime_error("Could not find any dance tracks.");
	
	gameMode(0);
	difficultyDelta(0); // hack to get initial level
		
}

/// Attempt to select next/previous game mode
void DanceGraph::gameMode(int direction) {
	// Cycling
	if (direction == 0) {
		m_curTrackIt = m_song.danceTracks.begin();
	} else if (direction > 0) {
		m_curTrackIt++;
		if (m_curTrackIt == m_song.danceTracks.end()) m_curTrackIt = m_song.danceTracks.begin();
	} else if (direction < 0) {
		if (m_curTrackIt == m_song.danceTracks.begin()) m_curTrackIt = (--m_song.danceTracks.end());
		else m_curTrackIt--;
	}
	// Determine how many arrow lines are needed
	m_gamingMode = m_curTrackIt->first;
	std::string gm = m_gamingMode;
	if (gm == "dance-single") m_pads = 4;
	else if (gm == "dance-double") m_pads = 8;
	else if (gm == "dance-couple") m_pads = 8;
	else if (gm == "dance-solo") m_pads = 6;
	else if (gm == "pump-single") m_pads =5 ;
	else if (gm == "pump-double") m_pads = 10;
	else if (gm == "pump-couple") m_pads = 10;
	else if (gm == "ez2-single") m_pads = 5;
	else if (gm == "ez2-double") m_pads = 10;
	else if (gm == "ex2-real") m_pads = 7;
	else if (gm == "para-single") m_pads = 5;
	else throw std::runtime_error("Unknown track " + gm);
}

/// Are we alive?
bool DanceGraph::dead(double time) const {
	return m_jointime == not_joined || time > (m_acttime + death_delay);
}

/// Get the difficulty as displayable string
std::string DanceGraph::getDifficultyString() const { return diffv[m_level]; }

/// Attempt to change the difficulty by a step
void DanceGraph::difficultyDelta(int delta) {
	int newLevel = m_level + delta;
	std::cout << "difficultyDelta called with " << delta << " (newLevel = " << newLevel << ")" << std::endl;
	if(newLevel >= DIFFICULTYCOUNT || newLevel < 0) return; // Out of bounds
	DanceTracks::const_iterator it = m_song.danceTracks.find(m_gamingMode);
	if(it->second.find((DanceDifficulty)newLevel) != it->second.end())
		difficulty((DanceDifficulty)newLevel);
	else
		difficultyDelta(delta + (delta < 0 ? -1 : 1));
}

/// Select a difficulty and construct DanceNotes and score normalizer for it
void DanceGraph::difficulty(DanceDifficulty level) {
	// TODO: error handling)
	m_notes.clear();
	DanceTrack const& track = m_song.danceTracks.find(m_gamingMode)->second.find(level)->second;
	for(Notes::const_iterator it = track.notes.begin(); it != track.notes.end(); it++)
		m_notes.push_back(DanceNote(*it));
	std::sort(m_notes.begin(), m_notes.end(), lessEnd()); // for engine's iterators
	m_notesIt = m_notes.begin();
//	std::cout << "Difficulty set to: " << level << std::endl;
	m_level = level;
	for(size_t i=0; i < 4; i++)
		m_activeNotes[i] = m_notes.end();
	m_scoreFactor = 1;
	if(m_notes.size() != 0)
		m_scoreFactor = 10000.0 / (50 * m_notes.size()); // maxpoints / (notepoint * notes)
	std::cout << "Scorefactor: " << m_scoreFactor << std::endl;
}

/// Handles input and some logic
void DanceGraph::engine() {
	double time = m_audio.getPosition();
	time -= config["audio/controller_delay"].f();

	// Notes gone by
	for (DanceNotes::iterator& it = m_notesIt; it != m_notes.end() && time > it->note.end + maxTolerance; it++) {
		if(!it->isHit) { // Missed
			std::cout << "(Engine) Missed note at time " << time
			  << "(note timing " << it->note.begin << ")" << std::endl;
			if (it->note.type != Note::MINE) m_streak = 0;
		} else { // Hit, add score
			if(it->note.type != Note::MINE) {
				std::cout << "Note correctly played.." << std::endl;
				m_score += it->score;
			}
			if(!it->releaseTime) it->releaseTime = time;
		}
	}

	// Holding button when mine comes?
	for (DanceNotes::iterator it = m_notesIt; it != m_notes.end() && time <= it->note.begin + maxTolerance; it++) {
		if(!it->isHit && it->note.type == Note::MINE && m_pressed[it->note.note] &&
		  it->note.begin >= time - maxTolerance && it->note.end <= time + maxTolerance) {
			std::cout << "Hit mine at " << time << "!" << std::endl;
			it->isHit = true;
			m_score -= points(0);
		}
	}

	// Handle all events
	for (input::Event ev; m_input.tryPoll(ev);) {
		// Handle joining and keeping alive
		if (m_jointime == not_joined) m_jointime = (time < 0.0 ? -join_delay : time); // join
		m_acttime = time;
		
		if(ev.button < 0 || ev.button > 3) continue; // ignore other than 4 buttons for now
		// Difficulty / mode selection
		if (time < m_jointime + join_delay) {
			if (ev.type == input::Event::PRESS) {
				if (ev.pressed[STEP_UP]) difficultyDelta(1);
				else if (ev.pressed[STEP_DOWN]) difficultyDelta(-1);
				else if (ev.pressed[STEP_LEFT]) gameMode(-1);
				else if (ev.pressed[STEP_RIGHT]) gameMode(1);
			}
		}
		// Gaming controls
		if (ev.type == input::Event::RELEASE) {
			m_pressed[ev.button] = false;
			dance(time, ev);
			m_pressed_anim[ev.button].setTarget(0.0);
		} else if (ev.type == input::Event::PRESS) {
			m_pressed[ev.button] = true;
			dance(time, ev);
			m_pressed_anim[ev.button].setValue(1.0);
		}
	}

	// Check if a long streak goal has been reached
	if (m_streak >= getNextBigStreak(m_bigStreak)) {
		m_streakPopup.setTarget(1.0);
		m_bigStreak = getNextBigStreak(m_bigStreak);
	}
}

/// Handles scoring and such
void DanceGraph::dance(double time, input::Event const& ev) {
	// Handle release events
	if(ev.type == input::Event::RELEASE) {
		DanceNotes::iterator it = m_activeNotes[ev.button];
		if(it != m_notes.end()) {
			if(!it->releaseTime && it->note.end > time + maxTolerance) {
				it->releaseTime = time;
				it->score = 0;
				std::cout << "Failed to hold note " << it->note.note << "! Begin: "
				  << it->note.begin << "; End: " << it->note.end << std::endl;
				m_streak = 0;
			}
		}
		return;
	}

	std::cout << "Hit button " << ev.button << " at " << time << std::endl;
	for (DanceNotes::iterator it = m_notesIt; it != m_notes.end() && time <= it->note.begin + maxTolerance; it++) {
		if(!it->isHit && time >= it->note.begin - maxTolerance && ev.button == it->note.note) {
			it->isHit = true;
			if (it->note.type != Note::MINE) {
				it->score = points(it->note.begin - time);
				it->accuracy = 1.0 - (std::abs(it->note.begin - time) / maxTolerance);
				m_streak++;
				if (m_streak > m_longestStreak) m_longestStreak = m_streak;
			} else { // Mine!
				m_score -= points(0);
				m_streak = 0;
			}
			m_activeNotes[ev.button] = it;
			break;
		}
	}
}


namespace {
	const float arrowSize = 0.4f; // Half width of an arrow
	const float one_arrow_tex_w = 1.0 / 8.0; // Width of a single arrow in texture coordinates
	
	/// Create a symmetric vertex pair of given data
	void vertexPair(int arrow_i, float x, float y, float ty) {
		glTexCoord2f(arrow_i * one_arrow_tex_w, ty); glVertex2f(x - arrowSize, y);
		glTexCoord2f((arrow_i+1) * one_arrow_tex_w, ty); glVertex2f(x + arrowSize, y);
	}

	glutil::Color& colorGlow(glutil::Color& c, double glow) {
		//c.a = std::sqrt(1.0 - glow);
		c.a = 1.0 - glow;
		c.r += glow *.5;
		c.g += glow *.5;
		c.b += glow *.5;
		return c;
	}
}

/// Draw a dance pad icon using the given texture
void DanceGraph::drawArrow(int arrow_i, Texture& tex, float x, float y, float scale, float ty1, float ty2) {
	glutil::Translation tr(x, y, 0.0f);
	if (scale != 1.0f) glScalef(scale, scale, scale);
	{
		UseTexture tblock(tex);
		glutil::Begin block(GL_TRIANGLE_STRIP);
		vertexPair(arrow_i, 0.0f, -arrowSize, ty1);
		vertexPair(arrow_i, 0.0f, arrowSize, ty2);
	}
	if (scale != 1.0f) glScalef(1.0f/scale, 1.0f/scale, 1.0f/scale);
}

/// Draw a mine note
void DanceGraph::drawMine(float x, float y, float rot, float scale) {
	glutil::Translation tr(x, y, 0.0f);
	if (scale != 1.0f) glScalef(scale, scale, scale);
	if (rot != 0.0f) glRotatef(rot, 0.0f, 0.0f, 1.0f);
	m_mine.draw();
	if (rot != 0.0f) glRotatef(-rot, 0.0f, 0.0f, 1.0f);
	if (scale != 1.0f) glScalef(1.0f/scale, 1.0f/scale, 1.0f/scale);
}

/// Draws the dance graph
void DanceGraph::draw(double time) {
	Dimensions dimensions(1.0); // FIXME: bogus aspect ratio (is this fixable?)
	dimensions.screenTop().middle(m_cx.get()).stretch(m_width.get(), 1.0);
	double offsetX = 0.5 * (dimensions.x1() + dimensions.x2());
	double frac = 0.75;  // Adjustable: 1.0 means fully separated, 0.0 means fully attached

	// Some matrix magic to get the viewport right
	{ glutil::PushMatrixMode pmm(GL_PROJECTION);
	{ glutil::Translation tr1(frac * 2.0 * offsetX, 0.0f, 0.0f);
	{ glutil::PushMatrixMode pmb(GL_MODELVIEW);
	{ glutil::Translation tr2((1.0 - frac) * offsetX, dimensions.y1(), 0.0f);
	{ float temp_s = dimensions.w() / 5.0f;
	  glutil::Scale sc1(temp_s, temp_s, temp_s);
	
	// Arrows on cursor
	glColor3f(1.0f, 1.0f, 1.0f);
	for (int arrow_i = 0; arrow_i < m_pads; ++arrow_i) {
		float x = -1.5f + arrow_i;
		float y = time2y(0.0);
		float l = m_pressed_anim[arrow_i].get();
		float s = (5.0 - l) / 5.0;
		drawArrow(arrow_i, m_arrows_cursor, x, y, s);
	}
	
	// Draw the notes
	for (DanceNotes::iterator it = m_notes.begin(); it != m_notes.end(); ++it) {
		if (it->note.end - time < past) continue;
		if (it->note.begin - time > future) continue;
		drawNote(*it, time); // Let's just do all the calculating in the sub, instead of passing them as a long list
	}

	// To test arrow coordinate positioning
	//for (float i = past; i <= future; i+=0.2) {
		//std::cout << i << ": " << time2y(i) << std::endl;
		//drawArrow(1, 0, time2y(i), 0.6);
	//}
	
	} //< reverse scale sc1
	} //< reverse trans tr2
	} //< reverse push pmb
	} //< reverse trans tr1
	} //< reverse push pmm
	
	drawInfo(time, offsetX, dimensions); // Go draw some texts and other interface stuff
	glColor3f(1.0f, 1.0f, 1.0f);
}

/// Draws a single note (or hold)
void DanceGraph::drawNote(DanceNote& note, double time) {
	float tBeg = note.note.begin - time;
	float tEnd = note.note.end - time;
	int arrow_i = note.note.note;
	bool mine = note.note.type == Note::MINE;
	float x = -1.5f + arrow_i;
	float s = 1.0;
	float ac = note.accuracy;
	float yBeg = time2y(tBeg);
	float yEnd = time2y(tEnd);
	glutil::Color c(1.0f, 1.0f, 1.0f);
	
	// Did we hit it?
	if (note.isHit && std::abs(tEnd) < maxTolerance && note.hitAnim.getTarget() == 0) {
		if (mine) note.hitAnim.setRate(1.0);
		note.hitAnim.setTarget(1.0, false);
	}
	double glow = note.hitAnim.get();
	
	if (yEnd - yBeg > arrowSize) {
		// Draw holds
		glColor4fv(c);
		if (note.isHit && note.releaseTime <= 0) { // The note is being held down
			yBeg = std::max(time2y(0.0), yBeg);
			yEnd = std::max(time2y(0.0), yEnd);
			glColor3f(1.0f, 1.0f, 1.0f);
		}
		if (note.releaseTime > 0) yBeg = time2y(note.releaseTime - time); // Oh noes, it got released!
		if (yEnd - yBeg > 0) {
			UseTexture tblock(m_arrows_hold);
			glutil::Begin block(GL_TRIANGLE_STRIP);
			// Draw end
			vertexPair(arrow_i, x, yEnd, 1.0f);
			float yMid = std::max(yEnd-arrowSize, yBeg+arrowSize);
			vertexPair(arrow_i, x, yMid, 2.0f/3.0f);
			// Draw middle
			vertexPair(arrow_i, x, yBeg+arrowSize, 1.0f/3.0f);
		}
		// Draw begin
		if (note.isHit && tEnd < 0.1) {
			glColor4fv(colorGlow(c,glow));
			s += glow;
		}
		drawArrow(arrow_i, m_arrows_hold, x, yBeg, s, 0.0f, 1.0f/3.0f);
	} else {
		// Draw short note
		if (mine) { // Mines need special handling
			c.a = 1.0 - glow; glColor4fv(c);
			s = 0.8f + glow * 0.5f;
			float rot = int(time*360 * (note.isHit ? 2.0 : 1.0) ) % 360; // They rotate!
			if (note.isHit) yBeg = time2y(0.0);
			drawMine(x, yBeg, rot, s);
		} else { // Regular arrows
			s += glow;
			glColor4fv(colorGlow(c, glow));
			drawArrow(arrow_i, m_arrows, x, yBeg, s);
		}
	}
	// Draw a text telling how well we hit
	if (glow > 0 && ac > 0 && !mine) {
		double s = 1.2 * arrowSize * (1.0 + glow);
		glColor3f(1.0f, 1.0f, 1.0f);
		std::string rank = "Horrible!";
		if (ac > .90) rank = "Perfect!";
		else if (ac > .80) rank = "Excellent!";
		else if (ac > .70) rank = "Great!";
		else if (ac > .60) rank = "Good!";
		else if (ac > .50) rank = "  OK!  ";
		else if (ac > .40) rank = "Poor!";
		else if (ac > .30) rank = " Bad! ";
		m_popupText->render(rank);
		m_popupText->dimensions().middle(x).center(time2y(0.0)).stretch(s,s/2.0);
		m_popupText->draw();
	}
}

/// Draw popups and other info texts
void DanceGraph::drawInfo(double time, double offsetX, Dimensions dimensions) {
	// Draw info
	if (time < m_jointime + join_delay) {
		m_text.dimensions.screenBottom(-0.075).middle(-0.09 + offsetX);
		m_text.draw("^ " + getDifficultyString() + " v");
		m_text.dimensions.screenBottom(-0.050).middle(-0.09 + offsetX);
		m_text.draw("< " + getGameMode() + " >");
	} else { // Draw scores
		m_text.dimensions.screenBottom(-0.35).middle(0.32 * dimensions.w() + offsetX);
		m_text.draw(boost::lexical_cast<std::string>(unsigned(getScore())));
		m_text.dimensions.screenBottom(-0.32).middle(0.32 * dimensions.w() + offsetX);
		m_text.draw(boost::lexical_cast<std::string>(unsigned(m_streak)) + "/" 
		  + boost::lexical_cast<std::string>(unsigned(m_longestStreak)));
	}
	// Draw streak pop-up for long streak intervals
	double streakAnim = m_streakPopup.get();
	if (streakAnim > 0.0) {
		double s = 0.15 * (1.0 + streakAnim);
		glColor4f(1.0f, 0.0f, 0.0f, 1.0 - streakAnim);
		m_popupText->render(boost::lexical_cast<std::string>(unsigned(m_bigStreak)) + "\nStreak!");
		m_popupText->dimensions().center(0.0).middle(offsetX).stretch(s,s);
		m_popupText->draw();
		if (streakAnim > 0.999) m_streakPopup.setTarget(0.0, true);
	}
}